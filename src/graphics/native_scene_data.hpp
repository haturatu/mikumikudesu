#pragma once

#include "graphics/device.hpp"

#include <mmd/animation.hpp>
#include <mmd/pmx.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dayo::graphics {

// These structures mirror the StructuredBuffer declarations in the
// MikuMikuDayo resources.hlsli/dayotypes.hlsli contract. They intentionally
// remain separate from PreviewMaterialGpu and NativeDeformedVertex: Preview
// has a different shader ABI and the latter is only the compact BLAS input.
struct alignas(16) NativeSceneVertex {
    float position[3]{};
    float normal[3]{};
    float tangent[3]{};
    float uv[2]{};
    float edge{};
    float exuv[16]{};
};
static_assert(sizeof(NativeSceneVertex) == 112);
static_assert(alignof(NativeSceneVertex) == 16);

struct alignas(16) NativeSceneMaterial {
    float diffuse[4]{};
    float specular[3]{};
    float shininess{};
    float ambient[3]{};
    float edgeColor[4]{};
    float edgeSize{};
    float textureAddValue[4]{};
    float sphereAddValue[4]{};
    float toonAddValue[4]{};
    float textureMulValue[4]{};
    float sphereMulValue[4]{};
    float toonMulValue[4]{};
    std::int32_t drawFlag{};
    std::int32_t tex{-1};
    std::int32_t spTex{-1};
    std::int32_t spmode{};
    std::int32_t toonFlag{};
    std::int32_t toonTex{-1};
    std::int32_t vertexCount{};
    std::int32_t reserved{};
};
static_assert(sizeof(NativeSceneMaterial) == 192);
static_assert(alignof(NativeSceneMaterial) == 16);

struct alignas(16) NativeSceneOidnInput {
    float color[3]{};
    float colorPadding{};
    float albedo[3]{};
    float albedoPadding{};
    float normal[3]{};
    float normalPadding{};
};
static_assert(sizeof(NativeSceneOidnInput) == 48);

struct alignas(16) NativeSceneMaterialFace {
    std::uint32_t start{};
    std::uint32_t count{};
    float totalArea{};
    std::uint32_t padding{};
};
static_assert(sizeof(NativeSceneMaterialFace) == 16);

struct alignas(16) NativeSceneWalkerAlias {
    std::uint32_t pair{};
    float probability{1.0F};
    float pdf{1.0F};
    std::uint32_t padding{};
};
static_assert(sizeof(NativeSceneWalkerAlias) == 16);

struct NativeSceneModelData {
    // Per-frame evaluated vertices consumed by raster and acceleration paths.
    std::vector<NativeSceneVertex> vertices;
    // Immutable bind-pose source consumed by deform shaders through RawVB.
    std::vector<NativeSceneVertex> rawVertices;
    std::vector<std::uint32_t> indices;
    std::vector<NativeSceneMaterial> materials;
    // One material index per triangle, matching Faces[model][face].
    std::vector<std::uint32_t> faces;
    std::vector<NativeSceneMaterialFace> materialFaces;
    std::vector<NativeSceneWalkerAlias> faceWalker;
    // Callers advance these generations when the corresponding CPU data
    // changes. NativeSceneModelRuntime uses them instead of scanning all
    // static bytes on every frame.
    std::uint64_t topologyGeneration{1};
    std::uint64_t materialGeneration{1};
};

// One material range of one evaluated model. The typed vertex/index buffers
// are carried with the range so the generic FX executor can draw models that
// are owned by different scene resources without guessing from the first
// writable texture or buffer.
struct NativeSceneDraw {
    handles::BufferHandle vertexBuffer{};
    handles::BufferHandle indexBuffer{};
    std::uint32_t modelIndex{};
    std::uint32_t materialIndex{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t instanceCount{1};
    std::uint32_t firstInstance{};
    std::int32_t vertexOffset{};
    bool buffer{};
    // CBuff1 values are part of the draw contract, not global frame state.
    std::uint32_t rasterizeOrder{};
    std::uint32_t deformIndex{};
    std::uint32_t deformOrder{};
};

// Converts the application's normalized Preview vertex representation to the
// upstream vertex ABI. The optional tangent is reconstructed from the local
// triangle/index data; additional UV channels are zero because Preview does
// not currently retain them in its compact vertex type.
[[nodiscard]] NativeSceneVertex makeNativeSceneVertex(const PreviewVertex& vertex) noexcept;

// Converts a PMX material and, when present, its animated scalar/vector state
// to the exact field order consumed by MMDMaterial in dayotypes.hlsli.
[[nodiscard]] NativeSceneMaterial
makeNativeSceneMaterial(const mmd::PmxMaterial& material,
                        const mmd::AnimatedModelFrame::Material* animated = nullptr) noexcept;

// Builds all per-model CPU buffers consumed by the canonical scene descriptor
// sets. The function is deterministic and does not allocate GPU resources;
// NativeSceneResourceGpuRuntime owns the corresponding device buffers.
[[nodiscard]] NativeSceneModelData
makeNativeSceneModelData(const mmd::PmxModel& model, std::span<const PreviewVertex> vertices,
                         std::span<const mmd::AnimatedModelFrame::Material> animatedMaterials = {},
                         mmd::PreviewNormalization normalization = {});

} // namespace dayo::graphics
