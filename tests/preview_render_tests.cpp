#include "core/model_probe.hpp"
#include "fx/fx_compiler.hpp"
#include "fx/fx_shader_compiler.hpp"
#include "graphics/fx_pipeline_runtime.hpp"
#include "graphics/subayai_deform.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/vulkan/vulkan_device.hpp"
#include "platform/window.hpp"
#include "ui/theme.hpp"

#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

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

bool createsD24S8StencilPipeline(dayo::graphics::VulkanDevice& device) {
    if (!device.supportsTextureFormat(dayo::graphics::PixelFormat::depth24Stencil8,
                                      dayo::graphics::ResourceUsage::depthWrite)) {
        std::cout << "SKIP: GPU does not support D24S8 depth attachments\n";
        return true;
    }

    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;

    dayo::fx::FxProgram program;
    program.hlsl = "float4 VS(uint id : SV_VertexID) : SV_Position { "
                   "return float4(float(id == 0), 0.0, 0.0, 1.0); }\n"
                   "float4 PS() : SV_Target0 { return float4(1.0, 0.0, 0.0, 1.0); }\n";
    dayo::core::EffectTexture color;
    color.name = "Color";
    color.format = "R8G8B8A8_UNORM";
    color.view = "RTV";
    program.textures.push_back(color);
    dayo::core::EffectTexture depth;
    depth.name = "Depth";
    depth.format = "D24_UNORM_S8_UINT";
    depth.view = "DSV";
    program.textures.push_back(depth);

    dayo::fx::FxRasterDispatch raster;
    raster.vertexShader = "VS";
    raster.pixelShader = "PS";
    raster.colorAttachments.push_back({.name = "Color", .clear = false, .clearValue = {}});
    raster.depthAttachment = dayo::core::EffectAttachment{.name = "Depth", .clear = false, .clearValue = {}};
    raster.graphics.depthStencil.depthEnable = true;
    raster.graphics.depthStencil.stencilEnable = true;
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "stencil-test";
    dispatch.kind = dayo::fx::FxOpKind::raster;
    dispatch.executable = std::move(raster);
    program.passes.push_back(std::move(dispatch));

    const auto context = dayo::fx::makeFxFrameContext(0.0F, 0, 64, 64, 0, 0, 0, 0, 1, 1);
    const auto plan = dayo::fx::FxCompiler{}.plan(program, context);
    const auto layout = device.createPipelineLayoutEx({});
    dayo::graphics::FxPipelineRuntime runtime;
    std::string error;
    const bool built = runtime.build(
        device, program, plan, compiler,
        [layout](const dayo::fx::FxDispatch&) {
            return std::optional<dayo::graphics::handles::PipelineLayoutHandle>{layout};
        },
        &error);
    runtime.reset();
    device.destroyPipelineLayoutEx(layout);
    if (!built)
        std::cerr << "FAIL: D24S8 stencil pipeline is invalid under Vulkan validation: " << error << '\n';
    return built;
}

bool typedVulkanFormatRoundTrips(dayo::graphics::VulkanDevice& device) {
    const std::array formats{
        dayo::graphics::PixelFormat::r8Unorm,     dayo::graphics::PixelFormat::r16Float,
        dayo::graphics::PixelFormat::r8Uint,      dayo::graphics::PixelFormat::r8Sint,
        dayo::graphics::PixelFormat::r16g16Snorm, dayo::graphics::PixelFormat::rgba16Uint,
        dayo::graphics::PixelFormat::rgba32Sint,
    };
    for (const auto format : formats) {
        const dayo::graphics::TextureResourceDesc desc{
            .dimension = dayo::graphics::TextureDimension::d2,
            .extent = {2, 2, 1},
            .format = format,
            .usage = dayo::graphics::ResourceUsage::sampledRead | dayo::graphics::ResourceUsage::transferDst |
                     dayo::graphics::ResourceUsage::transferSrc,
        };
        dayo::graphics::handles::TextureHandle texture;
        try {
            texture = device.createTextureEx(desc);
            std::vector<std::uint8_t> input(dayo::graphics::pixelFormatByteSize(format) * 4U);
            for (std::size_t index = 0; index < input.size(); ++index)
                input[index] = static_cast<std::uint8_t>((index * 37U + 11U) & 0xffU);
            for (unsigned cycle = 0; cycle < 4; ++cycle) {
                input.front() = static_cast<std::uint8_t>(cycle * 19U + 3U);
                device.uploadTextureEx(texture, input, 0, 0);
                if (device.readbackTextureEx(texture, 0, 0) != input)
                    throw std::runtime_error("repeated upload/readback payload mismatch");
            }
            device.destroyTextureEx(texture);
        } catch (const std::exception& exception) {
            if (texture.valid()) {
                try {
                    device.destroyTextureEx(texture);
                } catch (...) {
                }
            }
            std::cerr << "FAIL: Vulkan typed image round trip for " << dayo::graphics::toString(format) << ": "
                      << exception.what() << '\n';
            return false;
        }
    }
    // Buffer readback must not write into staging subsequently used by texture uploads.
    dayo::graphics::handles::BufferHandle buffer;
    try {
        buffer = device.createBufferEx(
            {.size = 64,
             .usage = dayo::graphics::ResourceUsage::transferSrc | dayo::graphics::ResourceUsage::transferDst});
        std::vector<std::byte> input(64, std::byte{0x37});
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            input.front() = static_cast<std::byte>(cycle + 1);
            device.uploadBufferEx(buffer, input);
            if (device.readbackBufferEx(buffer, 0, input.size()) != input)
                throw std::runtime_error("repeated buffer upload/readback payload mismatch");
        }
        device.destroyBufferEx(buffer);
    } catch (const std::exception& exception) {
        if (buffer.valid())
            device.destroyBufferEx(buffer);
        std::cerr << "FAIL: Vulkan buffer upload/readback round trip: " << exception.what() << '\n';
        return false;
    }
    return true;
}

bool batchedTextureReadback(dayo::graphics::VulkanDevice& device) {
    using namespace dayo::graphics;
    const auto cube = device.createTextureEx(
        {.dimension = TextureDimension::cube,
         .extent = {8, 8, 1},
         .format = PixelFormat::rgba16Float,
         .mipLevels = 4,
         .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst | ResourceUsage::transferSrc});
    const auto scalar = device.createTextureEx(
        {.extent = {1, 1, 1},
         .format = PixelFormat::r8Unorm,
         .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst | ResourceUsage::transferSrc});
    try {
        std::vector<TextureReadbackRequest> requests;
        std::vector<std::uint8_t> expected;
        for (std::uint32_t mip = 0; mip < 4; ++mip) {
            for (std::uint32_t face = 0; face < 6; ++face) {
                const auto size = 8U >> mip;
                std::vector<std::uint8_t> bytes(size * size * 8U, static_cast<std::uint8_t>(mip * 6 + face));
                device.uploadTextureEx(cube, bytes, mip, face);
                requests.push_back({cube, mip, face});
                expected.insert(expected.end(), bytes.begin(), bytes.end());
            }
        }
        if (device.readbackTextureSubresources(requests) != expected)
            throw std::runtime_error("cube face/mip batch order differs from uploaded payloads");
        const std::array<std::uint8_t, 1> scalarBytes{0x51};
        device.uploadTextureEx(scalar, scalarBytes, 0, 0);
        // A one-byte texture followed by RGBA16F requires staging padding, excluded from the output.
        const std::array mixed{TextureReadbackRequest{scalar, 0, 0}, TextureReadbackRequest{cube, 3, 5},
                               TextureReadbackRequest{scalar, 0, 0}};
        std::vector<std::uint8_t> mixedExpected{0x51};
        mixedExpected.insert(mixedExpected.end(), 8, 23);
        mixedExpected.push_back(0x51);
        if (device.readbackTextureSubresources(mixed) != mixedExpected ||
            !device.readbackTextureSubresources({}).empty())
            throw std::runtime_error("mixed-format batch padding or empty batch is incorrect");
        const std::array invalid{TextureReadbackRequest{cube, 0, 0}, TextureReadbackRequest{cube, 4, 0}};
        bool rejected = false;
        try {
            static_cast<void>(device.readbackTextureSubresources(invalid));
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        if (!rejected || device.readbackTextureSubresources(mixed) != mixedExpected)
            throw std::runtime_error("invalid batch was not rejected before recording GPU work");
        device.destroyTextureEx(cube);
        device.destroyTextureEx(scalar);
        return true;
    } catch (const std::exception& exception) {
        device.destroyTextureEx(cube);
        device.destroyTextureEx(scalar);
        std::cerr << "FAIL: batched Vulkan texture readback: " << exception.what() << '\n';
        return false;
    }
}

bool dedicatedStagingReadback(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::handles::TextureHandle texture;
    try {
        // Exceed the 64 MiB upload ring and verify the separate readback
        // allocation survives the GPU completion wait.
        const dayo::graphics::TextureResourceDesc desc{
            .dimension = dayo::graphics::TextureDimension::d2,
            .extent = {4097, 4096, 1},
            .format = dayo::graphics::PixelFormat::rgba8Unorm,
            .usage = dayo::graphics::ResourceUsage::sampledRead | dayo::graphics::ResourceUsage::transferDst |
                     dayo::graphics::ResourceUsage::transferSrc,
        };
        std::vector<std::uint8_t> input(4097U * 4096U * 4U, 0x5a);
        input.front() = 0x17;
        input.back() = 0xe3;
        texture = device.createTextureEx(desc);
        device.uploadTextureEx(texture, input, 0, 0);
        const auto output = device.readbackTextureEx(texture, 0, 0);
        device.destroyTextureEx(texture);
        return input == output;
    } catch (const std::exception& exception) {
        if (texture.valid()) {
            try {
                device.destroyTextureEx(texture);
            } catch (...) {
            }
        }
        std::cerr << "FAIL: dedicated Vulkan staging readback: " << exception.what() << '\n';
        return false;
    }
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

// Keep the model external; the default fixture reproduces Vivian vertex 9446
// and ten adjoining triangles without requiring the original PMX in CI.
bool zeroNormalVerticesStayInModelSpace(dayo::graphics::VulkanDevice& device) {
    constexpr Float3 center{0.0F, 16.691345F, 0.785809F};
    std::vector<PreviewVertex> vertices(11);
    std::vector<std::uint32_t> indices;
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        auto& vertex = vertices[index];
        std::copy(center.begin(), center.end(), vertex.position);
        vertex.normal[2] = 1.0F;
        vertex.uv[0] = 0.927500F;
        vertex.uv[1] = 0.342600F;
        vertex.bones[0] = 8;
        vertex.bones[1] = 9;
        vertex.weights[0] = 0.835283F;
        vertex.weights[1] = 0.164717F;
        if (index != 0) {
            const auto angle = static_cast<float>(index - 1) * 2.0F * std::numbers::pi_v<float> / 10.0F;
            vertex.position[0] += 0.6F * std::cos(angle);
            vertex.position[1] += 0.6F * std::sin(angle);
            indices.insert(indices.end(),
                           {0, static_cast<std::uint32_t>(index % 10 + 1), static_cast<std::uint32_t>(index)});
        }
    }
    std::size_t zeroIndex = 0;
    if (const auto* path = std::getenv("DAYO_TEST_VIVIAN_PMX")) {
        const auto model = dayo::core::loadPmxModel(path);
        constexpr std::size_t vivianIndex = 9446;
        if (model.vertices.size() != 31709 || model.indices.size() != 117111 ||
            model.vertices.at(vivianIndex).normal != Float3{}) {
            std::cerr << "FAIL: Vivian PMX does not match the reported zero-normal fixture\n";
            return false;
        }
        vertices.clear();
        for (const auto& source : model.vertices) {
            PreviewVertex vertex;
            std::copy(source.position.begin(), source.position.end(), vertex.position);
            std::copy(source.normal.begin(), source.normal.end(), vertex.normal);
            std::copy(source.uv.begin(), source.uv.end(), vertex.uv);
            std::copy(source.bones.begin(), source.bones.end(), vertex.bones);
            std::copy(source.weights.begin(), source.weights.end(), vertex.weights);
            vertices.push_back(vertex);
        }
        indices.clear();
        for (std::size_t offset = 0; offset < model.indices.size(); offset += 3) {
            const auto triangle = std::span(model.indices).subspan(offset, 3);
            if (std::ranges::find(triangle, vivianIndex) != triangle.end())
                indices.insert(indices.end(), triangle.begin(), triangle.end());
        }
        if (indices.size() != 30) {
            std::cerr << "FAIL: Vivian zero-normal vertex is not shared by ten triangles\n";
            return false;
        }
        zeroIndex = vivianIndex;
        std::cout << "INFO: testing Vivian.pmx vertex 9446 and its ten original triangles\n";
    }
    std::size_t boneCount = 10;
    for (const auto& vertex : vertices)
        for (const auto bone : vertex.bones)
            if (bone >= 0)
                boneCount = std::max(boneCount, static_cast<std::size_t>(bone) + 1);
    std::vector<PreviewBoneTransform> bones(boneCount);
    bones[8].translation[0] = 0.15F;
    bones[9].translation[1] = -0.1F;
    PreviewMaterial material;
    material.doubleSided = true;
    material.edgeEnabled = true;
    material.edgeSize = 0.02F;
    material.edgeColor[3] = 1.0F;
    const std::array materials{material};
    const std::array<PreviewDraw, 1> draws{{{0, static_cast<std::uint32_t>(indices.size()), 0}}};
    dayo::graphics::PreviewScene scene;
    scene.backgroundEnabled = false;
    scene.cameraDistance = 2.0F;
    std::copy(center.begin(), center.end(), scene.target);
    scene.cameraRotation[0] = 0.2F;
    scene.cameraRotation[1] = -0.15F;
    const auto render = [&]() {
        device.uploadPreviewMesh(vertices, indices);
        device.updatePreviewBones(bones);
        device.updatePreviewMaterials(materials);
        device.updatePreviewDraws(draws);
        return device.renderToImage({64, 64});
    };
    // Exercise CPU-deformed input and every preview GPU skinning mode, with
    // both projections and the outline pass. Only the normal changes.
    for (std::uint32_t mode = 0; mode < 6; ++mode) {
        for (const bool perspective : {false, true}) {
            scene.perspective = perspective;
            scene.outlineEnabled = !perspective;
            scene.debugFlags = perspective ? dayo::graphics::previewDebugNormals : 0U;
            device.updatePreviewScene(scene);
            for (auto& vertex : vertices) {
                vertex.gpuSkinning = mode == 0 ? 0U : 1U;
                vertex.skinningType = mode == 0 ? 0U : mode - 1;
                if (mode == 1) {
                    vertex.weights[0] = 1.0F;
                    vertex.weights[1] = 0.0F;
                } else {
                    vertex.weights[0] = 0.835283F;
                    vertex.weights[1] = 0.164717F;
                }
            }
            auto& normal = vertices[zeroIndex].normal;
            normal[0] = normal[1] = 0.0F;
            normal[2] = 1.0F;
            const auto reference = render();
            std::size_t visiblePixels = 0;
            for (std::size_t offset = 0; offset < reference.pixels.size(); offset += 4)
                visiblePixels += reference.pixels[offset] < 250U ? 1U : 0U;
            if (visiblePixels < 10) {
                std::cerr << "FAIL: zero-normal reference geometry is not visible\n";
                return false;
            }
            for (const float length : {0.0F, 1e-8F}) {
                normal[2] = length;
                if (render().pixels != reference.pixels) {
                    std::cerr << "FAIL: zero/tiny normal changed model rendering, mode=" << mode
                              << ", perspective=" << perspective << ", normal=" << length << '\n';
                    return false;
                }
            }
        }
    }
    resetPreviewScene(device);
    return true;
}

bool backgroundUsesExplicitPass(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> color{32, 96, 192, 255};
    const std::array<PreviewTexture, 1> textures{{{1, 1, color, false}}};
    device.uploadPreviewBackground(textures);
    dayo::graphics::PreviewScene scene;
    scene.screenSource = dayo::graphics::PreviewScene::ScreenSource::backgroundImage;
    scene.target[1] = 16.691345F; // Keep model geometry outside the view.
    scene.cameraRotation[0] = 0.5F;
    scene.debugMaterial = 99; // Model isolation must not hide the background.
    scene.debugFlags = dayo::graphics::previewDebugNormals;
    device.updatePreviewScene(scene);
    const auto vertices = makeFlatTriangle();
    const auto image = renderCase(device, vertices, std::array<PreviewBoneTransform, 1>{});
    for (std::size_t offset = 0; offset < image.pixels.size(); offset += 4) {
        if (!std::equal(color.begin(), color.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(offset))) {
            std::cerr << "FAIL: explicit background pass did not cover the framebuffer with its texture\n";
            return false;
        }
    }
    const std::array<std::uint8_t, 8> stripes{255, 0, 0, 255, 0, 0, 255, 255};
    const std::array<PreviewTexture, 1> stripedTextures{{{1, 2, stripes, false}}};
    device.uploadPreviewBackground(stripedTextures);
    const auto stripedImage = device.renderToImage({64, 64});
    if (stripedImage.pixels[0] != 255 || stripedImage.pixels[2] != 0 || stripedImage.pixels[(63U * 64U) * 4U] != 0 ||
        stripedImage.pixels[(63U * 64U) * 4U + 2U] != 255) {
        std::cerr << "FAIL: display background orientation or color changed during compositing\n";
        return false;
    }
    device.uploadPreviewBackground({});
    resetPreviewScene(device);
    return true;
}

bool modelPositiveYAppearsAboveCenter(dayo::graphics::VulkanDevice& device) {
    auto vertices = makeFlatTriangle();
    for (auto& vertex : vertices) {
        vertex.position[0] *= 0.3F;
        vertex.position[1] = vertex.position[1] * 0.3F + 0.7F;
    }
    PreviewMaterial material;
    material.diffuse[0] = 1.0F;
    material.diffuse[1] = material.diffuse[2] = 0.0F;
    std::fill_n(material.ambient, 3, 1.0F);
    device.uploadPreviewMesh(vertices, std::array<std::uint32_t, 3>{0, 2, 1});
    device.updatePreviewMaterials(std::array{material});
    device.updatePreviewDraws(std::array<PreviewDraw, 1>{{{0, 3, 0}}});
    for (const bool perspective : {true, false}) {
        dayo::graphics::PreviewScene scene;
        scene.cameraDistance = 3.0F;
        scene.perspective = perspective;
        scene.backgroundEnabled = false;
        device.updatePreviewScene(scene);
        const auto image = device.renderToImage({64, 64});
        std::size_t top = 0, bottom = 0;
        for (std::uint32_t y = 0; y < image.height; ++y)
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
                if (image.pixels[offset] > 150 && image.pixels[offset + 1] < 80)
                    (y < image.height / 2 ? top : bottom)++;
            }
        if (top == 0 || bottom != 0) {
            std::cerr << "FAIL: positive model Y did not appear above the image center\n";
            return false;
        }
    }
    resetPreviewScene(device);
    return true;
}

bool generatedEnvironmentDirectionsAndExposureAgree(dayo::graphics::VulkanDevice& device) {
    using namespace dayo::graphics;
    NativeEnvironmentBackend backend(
        device, {device.nativeEnvironmentEquirectPipeline(), device.nativeEnvironmentEquirectLayout(),
                 device.nativeEnvironmentPrefilterPipeline(), device.nativeEnvironmentPrefilterLayout()});
    dayo::core::ImageData image{.width = 32,
                                .height = 16,
                                .channels = 4,
                                .type = dayo::core::PixelType::half16,
                                .space = dayo::core::ColorSpace::linear,
                                .bytes = {}};
    image.bytes.resize(image.pixelCount() * 8U);
    const auto red = [](std::uint32_t x, std::uint32_t y) {
        if (x >= 14 && x <= 17 && y >= 6 && y <= 9)
            return 16.0F;                               // Bright +X window.
        return x == 0 ? 1.0F : (x == 31 ? 3.0F : 0.0F); // Asymmetric longitude seam.
    };
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x)
            for (std::uint32_t c = 0; c < 4; ++c) {
                const auto bits = dayo::core::floatToHalf(c == 0 ? red(x, y) : (c == 3 ? 1.0F : 0.0F));
                std::memcpy(image.bytes.data() + ((y * image.width + x) * 4U + c) * 2U, &bits, 2U);
            }
    const auto first = backend.regenerateImage({.source = "synthetic"}, image);
    const auto render = [&] {
        device.setNativeFrameRecorderForComputeTest(
            [&](CommandList& commands, const RenderTargetDesc&) -> std::optional<NativeFrameOutput> {
                backend.record(commands);
                return std::nullopt;
            });
        static_cast<void>(device.renderToImage({8, 8}));
        device.setNativeFrameRecorder({});
        device.selectRenderer(RendererKind::preview);
    };
    render();
    const auto readRed = [](const std::vector<std::uint8_t>& bytes, std::size_t pixel) {
        std::uint16_t bits{};
        std::memcpy(&bits, bytes.data() + pixel * 8U, 2U);
        return dayo::core::halfToFloat(bits);
    };
    const auto positive = device.readbackTextureEx(first.cubemap, 0, 0);
    const auto negative = device.readbackTextureEx(first.cubemap, 0, 1);
    const auto center =
        static_cast<std::size_t>(backend.faceSize() / 2U) * backend.faceSize() + backend.faceSize() / 2U;
    const auto specularPositive = device.readbackTextureEx(first.prefiltered, 0, 0);
    const auto specularNegative = device.readbackTextureEx(first.prefiltered, 0, 1);
    if (first.sphericalHarmonics[9] <= 0 || readRed(positive, center) <= readRed(negative, center) ||
        readRed(specularPositive, center) <= readRed(specularNegative, center)) {
        std::cerr << "FAIL: SH and generated cubemap disagree on the bright +X window\n";
        return false;
    }
    // Compare the -X face with a CPU bilinear lookup that wraps longitude.
    // Both sides of the seam must blend first/last columns rather than clamp.
    const auto size = backend.faceSize();
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const float px = (static_cast<float>(x) + 0.5F) / static_cast<float>(size) * 2.0F - 1.0F;
            const float py = (static_cast<float>(y) + 0.5F) / static_cast<float>(size) * 2.0F - 1.0F;
            const float length = std::sqrt(1.0F + px * px + py * py);
            const float u = std::atan2(px, -1.0F) / (2.0F * std::numbers::pi_v<float>)+0.5F;
            const float v = 0.5F - std::asin(-py / length) / std::numbers::pi_v<float>;
            const float sx = u * static_cast<float>(image.width) - 0.5F,
                        sy = v * static_cast<float>(image.height) - 0.5F;
            const auto ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
            float expected = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const auto wrapped = static_cast<std::uint32_t>((ix + dx + 32) % 32);
                    const auto clamped = static_cast<std::uint32_t>(std::clamp(iy + dy, 0, 15));
                    const float wx = dx ? sx - std::floor(sx) : 1.0F - (sx - std::floor(sx));
                    const float wy = dy ? sy - std::floor(sy) : 1.0F - (sy - std::floor(sy));
                    expected += red(wrapped, clamped) * wx * wy;
                }
            if (std::abs(readRed(negative, static_cast<std::size_t>(y) * size + x) - expected) > 0.03F) {
                std::cerr << "FAIL: environment conversion did not filter across the longitude seam\n";
                return false;
            }
        }
    const auto second = backend.regenerateImage({.source = "synthetic", .exposure = 2.0F}, image);
    render();
    const auto doubled = device.readbackTextureEx(second.cubemap, 0, 0);
    if (std::abs(second.sphericalHarmonics[9] - first.sphericalHarmonics[9] * 2.0F) > 1e-5F ||
        std::abs(readRed(doubled, center) - readRed(positive, center) * 2.0F) > 0.03F) {
        std::cerr << "FAIL: exposure did not scale both SH and cubemap radiance\n";
        return false;
    }
    return true;
}

bool environmentLightingStaysInWorldSpace(dayo::graphics::VulkanDevice& device) {
    using namespace dayo::graphics;
    const auto cube = device.createTextureEx({.dimension = TextureDimension::cube,
                                              .extent = {1, 1, 1},
                                              .format = PixelFormat::rgba8Unorm,
                                              .mipLevels = 1,
                                              .arrayLayers = 1,
                                              .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst});
    const std::array<std::uint8_t, 4> black{0, 0, 0, 255};
    for (std::uint32_t face = 0; face < 6; ++face)
        device.uploadTextureEx(cube, black, 0, face);
    PreviewEnvironment environment{.prefiltered = cube, .mipLevels = 1};
    environment.sphericalHarmonics[6] = environment.sphericalHarmonics[7] = environment.sphericalHarmonics[8] = 2.0F;
    device.updatePreviewEnvironment(environment);
    PreviewScene scene;
    scene.backgroundEnabled = false;
    std::fill_n(scene.lightColor, 3, 0.0F);
    device.updatePreviewScene(scene);
    PreviewMaterial material;
    material.doubleSided = true;
    material.diffuse[0] = material.diffuse[1] = material.diffuse[2] = 0.5F;
    const auto reference = centerPixel(renderMaterial(device, {}, material));
    scene.cameraRotation[1] = 0.7F;
    device.updatePreviewScene(scene);
    const auto rotated = centerPixel(device.renderToImage({64, 64}));
    device.updatePreviewEnvironment({});
    device.waitIdle();
    device.destroyTextureEx(cube);
    resetPreviewScene(device);
    if (std::abs(static_cast<int>(reference[0]) - static_cast<int>(rotated[0])) > 3 || reference[0] < 80) {
        std::cerr << "FAIL: camera rotation changes world-space diffuse IBL\n";
        return false;
    }
    return true;
}

bool transparentCardsDoNotCastSolidShadows(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.backgroundEnabled = false;
    std::fill_n(scene.lightDirection, 3, 0.0F);
    scene.lightDirection[2] = 1.0F;
    device.updatePreviewScene(scene);
    PreviewMaterial receiver;
    receiver.doubleSided = true;
    auto triangle = makeFlatTriangle();
    for (auto& vertex : triangle)
        vertex.normal[2] = -1.0F;
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    device.uploadPreviewMesh(triangle, indices);
    device.uploadPreviewTextures({});
    device.updatePreviewMaterials(std::array{receiver});
    device.updatePreviewDraws(std::array<PreviewDraw, 1>{{{0, 3, 0}}});
    const auto reference = centerPixel(device.renderToImage({64, 64}));
    std::vector<PreviewVertex> vertices(triangle.begin(), triangle.end());
    for (auto vertex : triangle) {
        vertex.position[2] = -0.5F;
        vertices.push_back(vertex);
    }
    const std::array<std::uint32_t, 6> cardIndices{0, 2, 1, 3, 5, 4};
    const std::array<std::uint8_t, 4> transparent{255, 255, 255, 0};
    const std::array<PreviewTexture, 1> textures{{{1, 1, transparent, true}}};
    auto card = receiver;
    card.textureSlot = 1;
    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, cardIndices);
    device.updatePreviewMaterials(std::array{receiver, card});
    device.updatePreviewDraws(std::array<PreviewDraw, 2>{{{0, 3, 0}, {3, 3, 1}}});
    const auto withCard = centerPixel(device.renderToImage({64, 64}));
    resetPreviewScene(device);
    if (std::abs(static_cast<int>(reference[0]) - static_cast<int>(withCard[0])) > 2) {
        std::cerr << "FAIL: transparent hair card casts a solid polygon shadow\n";
        return false;
    }
    return true;
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
    scene.lightDirection[0] = scene.lightDirection[1] = 0.0F;
    scene.lightDirection[2] = 1.0F;
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
    auto vertices = makeFlatTriangle();
    for (auto& vertex : vertices)
        vertex.normal[2] = -1.0F; // Face the camera and the incident light.
    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, std::array<std::uint32_t, 3>{0, 2, 1});
    device.updatePreviewMaterials(std::array{material});
    device.updatePreviewDraws(std::array<PreviewDraw, 1>{{{0, 3, 0}}});
    const auto pixel = centerPixel(device.renderToImage({64, 64}));
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
    for (auto& material : materials)
        std::fill_n(material.ambient, 3, 1.0F);
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
    for (auto& material : materials)
        std::fill_n(material.ambient, 3, 1.0F);
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
    for (auto& material : materials)
        std::fill_n(material.ambient, 3, 1.0F);
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
    std::fill_n(materials[0].ambient, 3, 1.0F);
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
    std::fill_n(materials[0].ambient, 3, 1.0F);
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

struct DayoSkinningGpuCase {
    std::string name;
    std::vector<PreviewVertex> vertices;
    std::vector<PreviewBoneTransform> bones;
    std::array<dayo::graphics::handles::BufferHandle, 4> inputs{};
    dayo::graphics::handles::BufferHandle output{};
    dayo::graphics::handles::DescriptorSetHandle descriptorSet{};
    Float3 expectedPosition{};
};

DayoSkinningGpuCase makeDayoSkinningGpuCase(std::string name, PreviewSkinningType skinningType,
                                            std::vector<PreviewBoneTransform> bones,
                                            const std::array<std::int32_t, 4>& boneIndices,
                                            const std::array<float, 4>& weights, const Float3& expectedPosition,
                                            std::uint32_t vertexCount = 1, const Float3& sdefCenter = {},
                                            const Float3& sdefHalfDelta = {}) {
    PreviewVertex vertex{};
    vertex.position[0] = 1.0F;
    vertex.position[1] = 2.0F;
    vertex.position[2] = 3.0F;
    vertex.normal[2] = 1.0F;
    std::copy(boneIndices.begin(), boneIndices.end(), vertex.bones);
    std::copy(weights.begin(), weights.end(), vertex.weights);
    std::copy(sdefCenter.begin(), sdefCenter.end(), vertex.sdefC);
    std::copy(sdefHalfDelta.begin(), sdefHalfDelta.end(), vertex.sdefHalfDelta);
    vertex.skinningType = static_cast<std::uint32_t>(skinningType);
    vertex.gpuSkinning = 1;
    return DayoSkinningGpuCase{.name = std::move(name),
                               .vertices = std::vector<PreviewVertex>(vertexCount, vertex),
                               .bones = std::move(bones),
                               .inputs = {},
                               .output = {},
                               .descriptorSet = {},
                               .expectedPosition = expectedPosition};
}

bool dayoSkinningGpuReadback(dayo::graphics::VulkanDevice& device) {
    if (!device.nativeDeformPipeline().valid() || !device.nativeDeformDescriptorLayout().valid()) {
        const auto* required = std::getenv("DAYO_UPSTREAM_REQUIRE_DXC");
        if (required != nullptr && std::string_view(required) == "1") {
            std::cerr << "FAIL: Dayo native deform compute pipeline is required but unavailable\n";
            return false;
        }
        std::cout << "INFO: Dayo native deform compute readback skipped; upstream compute pipeline is unavailable\n";
        return true;
    }

    const auto translatedBone = [](float x, float y, float z) {
        PreviewBoneTransform bone{};
        bone.translation[0] = x;
        bone.translation[1] = y;
        bone.translation[2] = z;
        return bone;
    };
    std::vector<DayoSkinningGpuCase> cases;
    cases.push_back(makeDayoSkinningGpuCase("BDEF1 valid bone", PreviewSkinningType::bdef1,
                                            {translatedBone(4.0F, -2.0F, 1.0F)}, {0, -1, -1, -1},
                                            {1.0F, 0.0F, 0.0F, 0.0F}, {5.0F, 0.0F, 4.0F}));
    cases.push_back(makeDayoSkinningGpuCase("BDEF2 partial invalid", PreviewSkinningType::bdef2,
                                            {translatedBone(8.0F, 4.0F, -4.0F)}, {0, -1, -1, -1},
                                            {0.25F, 0.75F, 0.0F, 0.0F}, {3.0F, 3.0F, 2.0F}));
    cases.push_back(makeDayoSkinningGpuCase("BDEF4 partial invalid", PreviewSkinningType::bdef4,
                                            {translatedBone(4.0F, 0.0F, 0.0F), translatedBone(0.0F, 8.0F, 0.0F)},
                                            {0, 1, -1, 17}, {0.25F, 0.25F, 0.25F, 0.25F}, {2.0F, 4.0F, 3.0F}));
    cases.push_back(makeDayoSkinningGpuCase("SDEF all invalid", PreviewSkinningType::sdef, {}, {-1, 42, -1, -1},
                                            {0.5F, 0.0F, 0.0F, 0.0F}, {1.0F, 2.0F, 3.0F}, 1, {0.5F, -0.25F, 0.1F},
                                            {0.2F, 0.1F, -0.3F}));
    cases.push_back(makeDayoSkinningGpuCase("QDEF partial invalid", PreviewSkinningType::qdef,
                                            {translatedBone(8.0F, 0.0F, 0.0F)}, {0, -1, -1, -1},
                                            {0.25F, 0.25F, 0.25F, 0.25F}, {3.0F, 2.0F, 3.0F}));
    cases.push_back(makeDayoSkinningGpuCase("BDEF1 all invalid", PreviewSkinningType::bdef1, {}, {-1, -1, -1, -1},
                                            {1.0F, 0.0F, 0.0F, 0.0F}, {1.0F, 2.0F, 3.0F}));
    cases.push_back(makeDayoSkinningGpuCase("QDEF all invalid", PreviewSkinningType::qdef, {}, {-1, -1, -1, -1},
                                            {0.25F, 0.25F, 0.25F, 0.25F}, {1.0F, 2.0F, 3.0F}));
    for (const auto count : {1U, 63U, 64U, 65U, 1023U, 1024U, 1025U})
        cases.push_back(makeDayoSkinningGpuCase("BDEF1 vertex-count boundary", PreviewSkinningType::bdef1,
                                                {translatedBone(0.5F, -0.25F, 1.0F)}, {0, -1, -1, -1},
                                                {1.0F, 0.0F, 0.0F, 0.0F}, {1.5F, 1.75F, 4.0F}, count));

    const auto release = [&device, &cases] {
        for (const auto& testCase : cases) {
            if (testCase.descriptorSet.valid()) {
                try {
                    device.destroyDescriptorSetEx(testCase.descriptorSet);
                } catch (...) {
                }
            }
            for (const auto input : testCase.inputs) {
                if (!input.valid())
                    continue;
                try {
                    device.destroyBufferEx(input);
                } catch (...) {
                }
            }
            if (testCase.output.valid()) {
                try {
                    device.destroyBufferEx(testCase.output);
                } catch (...) {
                }
            }
        }
    };
    bool success = true;
    try {
        const std::array<PreviewBoneTransform, 1> emptyBones{};
        const std::array<dayo::graphics::PreviewMorphDelta, 1> emptyMorphs{};
        const std::array<float, 1> emptyWeights{};
        const auto uploadInput = [&device](std::span<const std::byte> bytes) {
            const auto buffer = device.createBufferEx(
                {.size = bytes.size(),
                 .usage = dayo::graphics::ResourceUsage::storageRead | dayo::graphics::ResourceUsage::transferDst});
            try {
                device.uploadBufferEx(buffer, bytes);
            } catch (...) {
                device.destroyBufferEx(buffer);
                throw;
            }
            return buffer;
        };
        for (auto& testCase : cases) {
            const std::span<const PreviewBoneTransform> bones =
                testCase.bones.empty() ? std::span<const PreviewBoneTransform>(emptyBones)
                                       : std::span<const PreviewBoneTransform>(testCase.bones);
            testCase.inputs[0] = uploadInput(std::as_bytes(std::span(testCase.vertices)));
            testCase.inputs[1] = uploadInput(std::as_bytes(bones));
            testCase.inputs[2] = uploadInput(std::as_bytes(std::span(emptyMorphs)));
            testCase.inputs[3] = uploadInput(std::as_bytes(std::span(emptyWeights)));
            testCase.output = device.createBufferEx({
                .size = testCase.vertices.size() * sizeof(dayo::graphics::NativeDeformedVertex),
                .usage = dayo::graphics::ResourceUsage::storageWrite | dayo::graphics::ResourceUsage::transferSrc,
                .cpuVisible = true,
            });
            const std::array bindings{
                dayo::graphics::DescriptorBindingEx{0, 0, testCase.inputs[0], {}, {}, {}, std::nullopt},
                dayo::graphics::DescriptorBindingEx{1, 0, testCase.inputs[1], {}, {}, {}, std::nullopt},
                dayo::graphics::DescriptorBindingEx{2, 0, testCase.inputs[2], {}, {}, {}, std::nullopt},
                dayo::graphics::DescriptorBindingEx{3, 0, testCase.inputs[3], {}, {}, {}, std::nullopt},
                dayo::graphics::DescriptorBindingEx{4, 0, testCase.output, {}, {}, {}, std::nullopt},
            };
            testCase.descriptorSet = device.allocateDescriptorSetEx(device.nativeDeformDescriptorLayout(), bindings);
        }

        device.setNativeFrameRecorderForComputeTest(
            [&cases, pipeline = device.nativeDeformPipeline()](
                dayo::graphics::CommandList& commands,
                const dayo::graphics::RenderTargetDesc&) -> std::optional<dayo::graphics::NativeFrameOutput> {
                for (const auto& testCase : cases) {
                    commands.bindPipelineEx(pipeline);
                    commands.bindDescriptorSetEx(testCase.descriptorSet);
                    const dayo::graphics::NativeDeformPushConstants constants{
                        .vertexCount = static_cast<std::uint32_t>(testCase.vertices.size()),
                        .boneCount = static_cast<std::uint32_t>(testCase.bones.size()),
                        .morphDeltaCount = 0,
                        .morphCount = 0,
                    };
                    commands.pushConstantsEx(std::as_bytes(std::span(&constants, 1)));
                    commands.dispatch((constants.vertexCount + 63U) / 64U, 1, 1);
                }
                commands.memoryBarrierEx();
                return std::nullopt;
            });
        static_cast<void>(device.renderToImage({16, 16}));
        device.waitIdle();
        device.setNativeFrameRecorder({});

        for (const auto& testCase : cases) {
            const auto bytes = device.readbackBufferEx(
                testCase.output, 0, testCase.vertices.size() * sizeof(dayo::graphics::NativeDeformedVertex));
            if (bytes.size() != testCase.vertices.size() * sizeof(dayo::graphics::NativeDeformedVertex)) {
                std::cerr << "FAIL: GPU skinning readback has wrong size for " << testCase.name << '\n';
                success = false;
                break;
            }
            for (std::size_t index = 0; index < testCase.vertices.size(); ++index) {
                dayo::graphics::NativeDeformedVertex actual{};
                std::memcpy(&actual, bytes.data() + index * sizeof(actual), sizeof(actual));
                for (std::size_t component = 0; component < testCase.expectedPosition.size(); ++component) {
                    if (!std::isfinite(actual.position[component]) ||
                        std::abs(actual.position[component] - testCase.expectedPosition[component]) > 0.002F) {
                        std::cerr << "FAIL: GPU Dayo skinning position mismatch in " << testCase.name << " vertex "
                                  << index << '\n';
                        success = false;
                        break;
                    }
                }
                if (!success)
                    break;
            }
            if (!success)
                break;
        }
    } catch (const std::exception& exception) {
        device.setNativeFrameRecorder({});
        std::cerr << "FAIL: Dayo native deform compute readback: " << exception.what() << '\n';
        success = false;
    }
    release();
    if (success)
        std::cout << "INFO: Dayo native deform compute readback passed " << cases.size() << " fixtures\n";
    return success;
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

#if DAYO_HAS_IMGUI
bool rendersInteractiveViewport(dayo::graphics::VulkanDevice& device) {
    const auto vertices = makeFlatTriangle();
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
    }};
    std::array<PreviewMaterial, 1> materials{};
    materials[0].textureSlot = 1;
    std::fill_n(materials[0].ambient, 3, 1.0F);
    const std::array<PreviewDraw, 1> draws{{{0, 3, 0}}};
    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);

    const std::array<dayo::graphics::RenderTargetDesc, 4> extents{{{48, 32}, {48, 32}, {32, 48}, {32, 48}}};
    for (const auto extent : extents) {
        device.beginUiFrame();
        ImGui::SetNextWindowSize({64.0F, 64.0F}, ImGuiCond_Always);
        ImGui::Begin("interactive viewport test", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
        device.setPreviewViewportExtent(extent);
        const auto preview = device.previewViewport();
        if (!preview || preview.width != extent.width || preview.height != extent.height) {
            ImGui::End();
            ImGui::EndFrame();
            return false;
        }
        ImGui::Image(ImTextureRef{static_cast<ImTextureID>(preview.textureId)},
                     {static_cast<float>(extent.width), static_cast<float>(extent.height)});
        ImGui::End();
        device.renderFrame();
    }
    const auto image = device.renderToImage({48, 32});
    const auto pixel = centerPixel(image);
    return pixel[0] > 200U && pixel[1] < 80U && pixel[2] < 80U;
}
#endif

bool recordsNativeOffscreenOutput(dayo::graphics::VulkanDevice& device) {
    if (!device.capabilities().hardwareSupportsSubayai())
        return true;

    device.setNativeRendererAvailability(true, false);
    device.selectRenderer(dayo::graphics::RendererKind::subayai);
    bool called = false;
    dayo::graphics::handles::TextureHandle output{};
    dayo::graphics::handles::TextureHandle mipTarget{};
    device.setNativeFrameRecorder(
        [&](dayo::graphics::CommandList& commands, const dayo::graphics::RenderTargetDesc& target) {
            called = true;
            mipTarget = device.createTextureEx({
                .dimension = dayo::graphics::TextureDimension::d2,
                .extent = {target.width, target.height, 1},
                .format = dayo::graphics::PixelFormat::rgba16Float,
                .mipLevels = 7,
                .arrayLayers = 1,
                .usage = dayo::graphics::ResourceUsage::sampledRead | dayo::graphics::ResourceUsage::colorAttachment |
                         dayo::graphics::ResourceUsage::transferSrc,
                .lifetime = dayo::graphics::ResourceLifetime::transient,
            });
            dayo::graphics::RenderingInfoEx mipRendering;
            mipRendering.colors.push_back(
                {.texture = mipTarget, .clear = true, .clearColor = {1.0F, 0.0F, 0.0F, 1.0F}, .mipLevel = 1});
            mipRendering.extent = {std::max(target.width / 2U, 1U), std::max(target.height / 2U, 1U), 1};
            commands.beginRenderingEx(mipRendering);
            commands.endRenderingEx();
            output = device.createTextureEx({
                .dimension = dayo::graphics::TextureDimension::d2,
                .extent = {target.width, target.height, 1},
                .format = dayo::graphics::PixelFormat::rgba16Float,
                .mipLevels = 1,
                .arrayLayers = 1,
                .usage = dayo::graphics::ResourceUsage::sampledRead | dayo::graphics::ResourceUsage::transferDst,
                .lifetime = dayo::graphics::ResourceLifetime::transient,
            });
            commands.clearTextureEx(output, {1.0F, 0.0F, 0.0F, 1.0F});
            return std::optional<dayo::graphics::NativeFrameOutput>{dayo::graphics::NativeFrameOutput{
                .texture = output,
                .extent = {target.width, target.height, 1},
                .format = dayo::graphics::PixelFormat::rgba16Float,
            }};
        });
    bool outputMatches = false;
    try {
        const auto image = device.renderToImage({64, 64});
        const auto pixel = centerPixel(image);
        outputMatches = pixel[0] > 200U && pixel[1] < 32U && pixel[2] < 32U && pixel[3] > 200U;
    } catch (...) {
        device.setNativeFrameRecorder({});
        device.setNativeRendererAvailability(false, false);
        device.selectRenderer(dayo::graphics::RendererKind::preview);
        if (output.valid())
            device.destroyTextureEx(output);
        if (mipTarget.valid())
            device.destroyTextureEx(mipTarget);
        throw;
    }
    device.setNativeFrameRecorder({});
    device.setNativeRendererAvailability(false, false);
    device.selectRenderer(dayo::graphics::RendererKind::preview);
    const auto mipBytes = mipTarget.valid() ? device.readbackTextureEx(mipTarget, 1, 0) : std::vector<std::uint8_t>{};
    const bool mipClearMatches = mipBytes.size() >= 8U && mipBytes[0] == 0U && mipBytes[1] == 0x3CU &&
                                 mipBytes[2] == 0U && mipBytes[3] == 0U && mipBytes[4] == 0U && mipBytes[5] == 0U &&
                                 mipBytes[6] == 0U && mipBytes[7] == 0x3CU;
    if (output.valid())
        device.destroyTextureEx(output);
    if (mipTarget.valid())
        device.destroyTextureEx(mipTarget);
    return called && outputMatches && mipClearMatches;
}

bool pipelinedReadbackPreservesFrames(dayo::graphics::VulkanDevice& device) {
    auto vertices = makeFlatTriangle();
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    device.uploadPreviewMesh(vertices, indices);
    device.uploadPreviewTextures({});
    std::array<PreviewMaterial, 1> materials{};
    std::fill_n(materials[0].ambient, 3, 1.0F);
    const std::array<PreviewDraw, 1> draws{{{0, 3, 0}}};
    device.updatePreviewDraws(draws);
    device.uploadPreviewMorphDeltas({});
    device.updatePreviewMorphWeights({});
    dayo::graphics::PreviewScene scene;
    scene.backgroundEnabled = false;
    scene.cameraDistance = 3.0F;
    device.updatePreviewScene(scene);
    std::array<dayo::core::ImageRgba8, 3> expected;
    const auto update = [&](std::size_t i) {
        materials[0].diffuse[0] = i == 0 ? 1.0F : 0.0F;
        materials[0].diffuse[1] = i == 1 ? 1.0F : 0.0F;
        materials[0].diffuse[2] = i == 2 ? 1.0F : 0.0F;
        device.updatePreviewMaterials(materials);
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        update(i);
        expected[i] = device.renderToImage({65, 63});
    }
    // Synchronous callers repeatedly recycle the first slot. Preserve its GPU write history.
    for (std::size_t cycle = 0; cycle < 12; ++cycle) {
        const auto index = cycle % expected.size();
        update(index);
        if (!imagesMatch(expected[index], device.renderToImage({65, 63})))
            return false;
    }
    std::array<std::uint64_t, 3> tickets{};
    for (std::size_t i = 0; i < tickets.size(); ++i) {
        update(i);
        tickets[i] = device.enqueueRenderToImage({65, 63});
    }
    bool rejectedFull = false;
    try {
        static_cast<void>(device.enqueueRenderToImage({65, 63}));
    } catch (const std::logic_error&) {
        rejectedFull = true;
    }
    // Replace a mesh and grow buffers while old frames remain outstanding.
    device.uploadPreviewMesh(vertices, indices);
    const std::array<PreviewBoneTransform, 32> bones{};
    device.updatePreviewBones(bones);
    const std::array<float, 32> weights{};
    device.updatePreviewMorphWeights(weights);
    bool ok = rejectedFull;
    for (std::size_t i = 0; i < tickets.size(); ++i)
        ok &= imagesMatch(expected[i], device.collectRenderedImage(tickets[i]).value());
    try {
        static_cast<void>(device.collectRenderedImage(tickets[0]));
        ok = false;
    } catch (const std::invalid_argument&) {
    }
    const auto small = device.enqueueRenderToImage({17, 19});
    const auto large = device.enqueueRenderToImage({71, 67});
    const auto smallImage = device.collectRenderedImage(small).value();
    const auto largeImage = device.collectRenderedImage(large).value();
    ok &= smallImage.width == 17 && smallImage.height == 19 && largeImage.width == 71 && largeImage.height == 67;
    if (!ok)
        std::cerr << "FAIL: readback ring lost frame state, dimensions, or ticket ownership\n";
    return ok;
}

} // namespace

int main(int argc, char** argv) {
#if DAYO_HAS_IMGUI
    ImGui::CreateContext();
    dayo::ui::applyEditorTheme(2.0F);
    const auto expected = ImGui::GetStyle();
    dayo::ui::applyEditorTheme(1.0F);
    dayo::ui::applyEditorTheme(1.5F);
    dayo::ui::applyEditorTheme(2.0F);
    const auto actual = ImGui::GetStyle();
    ImGui::DestroyContext();
    if (actual.WindowBorderSize != expected.WindowBorderSize || actual.ChildBorderSize != expected.ChildBorderSize ||
        actual.IndentSpacing != expected.IndentSpacing || actual.GrabMinSize != expected.GrabMinSize ||
        actual.DisplayWindowPadding.x != expected.DisplayWindowPadding.x ||
        actual.FramePadding.y != expected.FramePadding.y) {
        std::cerr << "FAIL: repeated DPI changes accumulate style metrics\n";
        return 1;
    }
#endif
    try {
        const auto window = dayo::platform::createWindow({"preview shader test", 64, 64, true});
        dayo::graphics::VulkanOptions options;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--async-compute")
                options.asyncCompute = true;
            if (std::string_view(argv[i]) == "--high")
                options.quality = dayo::graphics::PreviewQuality::high;
        }
        dayo::graphics::VulkanDevice device(*window, true, options);
        dayo::graphics::PreviewScene scene;
        scene.cameraDistance = 3.0F;
        scene.backgroundEnabled = false;
        device.updatePreviewScene(scene);
        if (!device.nativeEnvironmentEquirectPipeline().valid() || !device.nativeEnvironmentEquirectLayout().valid() ||
            !device.nativeEnvironmentPrefilterPipeline().valid() ||
            !device.nativeEnvironmentPrefilterLayout().valid()) {
            std::cerr << "FAIL: native environment compute pipelines were not initialized\n";
            return 1;
        }
        if (!pipelinedReadbackPreservesFrames(device))
            return 1;
        if (!generatedEnvironmentDirectionsAndExposureAgree(device))
            return 1;
        if (!recordsNativeOffscreenOutput(device)) {
            std::cerr << "FAIL: native renderer was not recorded for offscreen output\n";
            return 1;
        }
        if (!createsD24S8StencilPipeline(device))
            return 1;
        if (!typedVulkanFormatRoundTrips(device) || !batchedTextureReadback(device) ||
            !dedicatedStagingReadback(device))
            return 1;
#if DAYO_HAS_IMGUI
        if (!rendersInteractiveViewport(device)) {
            std::cerr << "FAIL: interactive preview viewport did not render or resize\n";
            return 1;
        }
#endif
        if (!zeroNormalVerticesStayInModelSpace(device) || !backgroundUsesExplicitPass(device))
            return 1;
        if (!modelPositiveYAppearsAboveCenter(device) || !environmentLightingStaysInWorldSpace(device) ||
            !transparentCardsDoNotCastSolidShadows(device))
            return 1;
        if (!runCase(device, PreviewSkinningType::sdef)) {
            std::cerr << "FAIL: GPU SDEF output differs from reference rendering\n";
            return 1;
        }
        if (!runCase(device, PreviewSkinningType::qdef)) {
            std::cerr << "FAIL: GPU QDEF output differs from reference rendering\n";
            return 1;
        }
        if (!dayoSkinningGpuReadback(device))
            return 1;
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
        device.waitIdle();
        if (device.validationErrorCount() != 0) {
            std::cerr << "FAIL: Preview emitted " << device.validationErrorCount() << " Vulkan validation errors\n";
            return 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: preview shader rendering: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
