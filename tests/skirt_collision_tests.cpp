#include "app/preview_conversion.hpp"
#include "core/skirt_collision.hpp"
#include "graphics/preview_gpu_scene.hpp"
#include "graphics/skirt_collision.hpp"
#include "graphics/subayai_deform.hpp"
#include "graphics/vulkan/vulkan_device.hpp"
#include "platform/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace {
using namespace dayo;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
float distance(core::Float3 p, const core::SkirtCapsule& c) {
    core::Float3 ab{}, delta{};
    float numerator = 0, denominator = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        ab[axis] = c.bMargin[axis] - c.aRadius[axis];
        numerator += (p[axis] - c.aRadius[axis]) * ab[axis];
        denominator += ab[axis] * ab[axis];
    }
    const float t = std::clamp(numerator / std::max(denominator, 1e-8F), 0.0F, 1.0F);
    for (std::size_t axis = 0; axis < 3; ++axis)
        delta[axis] = p[axis] - c.aRadius[axis] - ab[axis] * t;
    return std::sqrt(std::inner_product(delta.begin(), delta.end(), delta.begin(), 0.0F)) - c.aRadius[3] - c.bMargin[3];
}
struct Result {
    std::vector<core::Float3> positions;
    std::vector<core::SkirtCapsule> capsules;
    core::SkirtCollisionStats stats;
    std::uint64_t gpuNanoseconds{};
};
class GpuFixture {
  public:
    graphics::VulkanDevice& device;
    graphics::SkirtCollisionRuntime runtime;
    graphics::handles::BufferHandle output;
    std::uint32_t stride;
    std::uint32_t normalOffset;
    std::size_t count;
    GpuFixture(graphics::VulkanDevice& d, std::size_t n, bool native)
        : device(d), stride(native ? 48U : 36U), normalOffset(native ? 16U : 12U), count(n) {
        output = device.createBufferEx({.size = count * stride,
                                        .usage = graphics::ResourceUsage::storageRead |
                                                 graphics::ResourceUsage::storageWrite |
                                                 graphics::ResourceUsage::transferSrc,
                                        .cpuVisible = true});
    }
    ~GpuFixture() {
        runtime.reset();
        device.destroyBufferEx(output);
    }
    Result run(std::span<const core::PmxVertex> vertices, const graphics::SkirtCollisionUpload& upload) {
        std::vector<std::byte> bytes(count * stride);
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(bytes.data() + i * stride, vertices[i].position.data(), 12);
            std::memcpy(bytes.data() + i * stride + normalOffset, vertices[i].normal.data(), 12);
        }
        device.uploadBufferEx(output, bytes, 0);
        device.setNativeFrameRecorderForComputeTest(
            [&](graphics::CommandList& commands,
                const graphics::RenderTargetDesc&) -> std::optional<graphics::NativeFrameOutput> {
                runtime.record(device, commands, output, upload, stride, normalOffset);
                return std::nullopt;
            });
        static_cast<void>(device.renderToImage({16, 16}));
        device.waitIdle();
        device.setNativeFrameRecorder({});
        const auto actual = device.readbackBufferEx(output, 0, bytes.size());
        Result result;
        result.stats = runtime.readStats();
        result.capsules = runtime.readCapsules();
        result.gpuNanoseconds = device.previewGpuNanoseconds();
        result.positions.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(result.positions[i].data(), actual.data() + i * stride, 12);
            core::Float3 normal;
            std::memcpy(normal.data(), actual.data() + i * stride + normalOffset, 12);
            for (float c : result.positions[i])
                require(std::isfinite(c), "collision produced nonfinite position");
            for (float c : normal)
                require(std::isfinite(c), "collision produced nonfinite normal");
        }
        return result;
    }
};
void topologyFixtures() {
    core::PmxModel model;
    model.vertices.resize(4);
    model.indices = {0, 1, 2, 1, 2, 3};
    core::PmxMaterial skirt;
    skirt.name = "スカート";
    skirt.indexCount = 3;
    core::PmxMaterial skin;
    skin.name = "skin";
    skin.indexCount = 3;
    model.materials = {skirt, skin};
    core::PmxRigidBody cloth;
    cloth.name = "skirt";
    cloth.mode = 1;
    cloth.group = 14;
    cloth.collisionMask = 1;
    core::PmxRigidBody leg;
    leg.name = "右足";
    leg.shape = 2;
    leg.size = {1, 2, 0};
    leg.collisionMask = 0xffff;
    leg.bone = 0;
    model.bones.resize(1);
    model.vertices[3].bones[0] = 0;
    model.rigidBodies = {cloth, leg};
    auto topology = core::identifySkirtCollision(model);
    require(topology.vertices.size() == 3 && topology.triangles.size() == 1 && topology.legBodies.size() == 1,
            "skirt extraction includes unrelated material or loses leg");
    require(std::ranges::none_of(topology.vertices, [](auto v) { return v.vertex == 3; }), "unrelated vertex selected");
    require(topology.legVertices == std::vector<std::uint32_t>{3}, "leg surface extraction included skirt vertices");
    model.rigidBodies[1].collisionMask = 0;
    require(core::identifySkirtCollision(model).empty(), "reverse collision mask was ignored");
    model.rigidBodies[1] = leg;
    model.rigidBodies[1].rotation = {0, 0, 1.570796327F};
    core::MmdPhysics physics(model);
    const auto capsules = core::makeSkirtCapsules(model, topology, physics, {}, {{2, 3, 4}, 2});
    require(std::abs(capsules[0].aRadius[0] - (-2)) < 1e-4F && std::abs(capsules[0].bMargin[0] - (-6)) < 1e-4F,
            "rotated capsule did not use local Y axis or model normalization");
    require(std::abs(capsules[0].aRadius[3] - 2) < 1e-4F, "capsule radius was not normalized");
}
void gpuFixtures(graphics::VulkanDevice& device) {
    std::vector<core::PmxVertex> points(4);
    points[0].position = {.1F, 0, 0};
    points[1].position = {.1F, .2F, .1F};
    points[2].position = {.1F, -.2F, .1F};
    points[3].position = {0, 0, 0};
    for (auto& p : points)
        p.normal = {1, 0, 0};
    const std::vector<core::SkirtVertex> vertices{
        {0, 0, 1, 0, 1, 1, {}}, {1, 1, 1, 0, 1, 1, {}}, {2, 2, 1, 0, 1, 1, {}}};
    const std::vector<std::array<std::uint32_t, 4>> triangles{{0, 1, 2, 0}};
    const std::vector<std::uint32_t> adjacency{0, 0, 0};
    const std::vector<core::SkirtCapsule> capsules{
        {{0, -1, 0, 1}, {0, 1, 0, .03F}, {0, -1, 0, 1}, {0, 1, 0, .03F}, {0, 0, 0, 1}}};
    graphics::SkirtCollisionUpload upload{vertices, triangles, adjacency, capsules, 1, 6};
    GpuFixture preview(device, points.size(), false), native(device, points.size(), true);
    auto a = preview.run(points, upload), b = native.run(points, upload);
    for (std::size_t i = 0; i < 3; ++i) {
        require(distance(a.positions[i], capsules[0]) >= -1e-4F, "vertex projection left deep capsule penetration");
        for (std::size_t axis = 0; axis < 3; ++axis)
            require(std::abs(a.positions[i][axis] - b.positions[i][axis]) < 1e-6F,
                    "Preview/Native collision layouts diverged");
    }
    require(a.positions[3] == points[3].position, "collision modified an unrelated vertex");
    require(a.stats.penetratingVertexCount == 0 && a.stats.totalCorrectionDistance > 1,
            "GPU diagnostics lost corrections");
    // Cross to the opposite side inside the capsule. Persistent history must
    // keep the original outward side, while a seek starts with the new side.
    for (std::size_t i = 0; i < 3; ++i)
        points[i].position[0] = -.1F;
    a = preview.run(points, upload);
    require(a.positions[0][0] > .5F, "deep penetration crossed to the wrong side");
    ++upload.historyGeneration;
    a = preview.run(points, upload);
    require(a.positions[0][0] < -.5F, "seek retained stale collision history");
    // All vertices are outside, while the centroid is inside the sphere.
    points[0].position = {2, 0, 0};
    points[1].position = {-1, std::numbers::sqrt3_v<float>, 0};
    points[2].position = {-1, -std::numbers::sqrt3_v<float>, 0};
    for (auto& p : points)
        p.normal = {0, 0, 1};
    std::vector<core::SkirtCapsule> sphere{
        {{0, 0, 0, 1}, {0, 0, 0, .03F}, {0, 0, 0, 1}, {0, 0, 0, .03F}, {0, 0, 0, 1}}};
    upload.capsules = sphere;
    upload.iterations = 0;
    ++upload.historyGeneration;
    const auto before = preview.run(points, upload);
    require(before.stats.penetratingVertexCount == 0 && before.stats.penetratingTriangleCount == 1,
            "triangle-only penetration was not detected");
    upload.iterations = 6;
    ++upload.historyGeneration;
    const auto after = preview.run(points, upload);
    require(after.stats.maxMeshPenetration < before.stats.maxMeshPenetration * .2F,
            "triangle constraints did not resolve interior contact");
    // Degenerate triangle and capsule axes must remain finite.
    for (auto& p : points)
        p.position = {0, 0, 0};
    ++upload.historyGeneration;
    static_cast<void>(preview.run(points, upload));
    std::cout
        << "GPU collision fixtures passed (vertex, triangle, history, seek, degenerate geometry, layout parity)\n";
}
void previewIntegration(graphics::VulkanDevice& device) {
    std::vector<graphics::PreviewVertex> vertices(4);
    const std::array<core::Float3, 4> points{{{.1F, 0, 0}, {.1F, .2F, .1F}, {.1F, -.2F, .1F}, {4, 0, 0}}};
    std::vector<core::PmxVertex> reference(4);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        std::copy(points[i].begin(), points[i].end(), vertices[i].position);
        vertices[i].normal[0] = 1;
        reference[i].position = points[i];
        reference[i].normal = {1, 0, 0};
        if (i < 3) {
            vertices[i].position[0] -= .5F;
            vertices[i].gpuSkinning = 1;
            vertices[i].bones[0] = 0;
        }
    }
    const std::vector<core::SkirtVertex> selected{
        {0, 0, 1, 0, 1, 1, {}}, {1, 1, 1, 0, 1, 1, {}}, {2, 2, 1, 0, 1, 1, {}}};
    const std::vector<std::array<std::uint32_t, 4>> triangles{{0, 1, 2, 0}};
    const std::vector<std::uint32_t> adjacency{0, 0, 0}, indices{0, 1, 2};
    const std::vector<core::SkirtCapsule> capsules{
        {{0, -1, 0, 1}, {0, 1, 0, .03F}, {0, -1, 0, 1}, {0, 1, 0, .03F}, {0, 0, 0, 1}}};
    GpuFixture fixture(device, 4, true);
    const auto expected = fixture.run(reference, {selected, triangles, adjacency, capsules, 1, 32});
    device.uploadPreviewMesh(vertices, indices);
    graphics::PreviewBoneTransform bone;
    bone.translation[0] = .5F;
    const std::array bones{bone};
    device.updatePreviewBones(bones);
    device.updatePreviewSkirtCollision(selected, triangles, adjacency, capsules, 1);
    static_cast<void>(device.renderToImage({16, 16}));
    const auto actual = device.readbackPreviewDeformedVertices();
    require(actual.size() == 4, "Preview collision output was not readable");
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t axis = 0; axis < 3; ++axis)
            require(std::abs(actual[i].position[axis] - expected.positions[i][axis]) < 1e-6F,
                    "Preview draw buffer did not receive the shared collision result");
    require(device.previewSkirtCollisionStats().penetratingVertexCount == 0, "Preview collision diagnostics differ");
    device.updatePreviewSkirtCollision({}, {}, {}, {}, 0);
    std::cout << "Preview post-skinning collision integration passed\n";
}

void nativeSkinningIntegration(graphics::VulkanDevice& device) {
    if (!device.nativeDeformPipeline().valid()) {
        std::cout << "Native skinning integration unavailable without DXC/upstream headers\n";
        return;
    }
    std::vector<graphics::PreviewVertex> source(4);
    std::vector<core::PmxVertex> skinned(4);
    const std::array<core::Float3, 4> points{{{.1F, 0, 0}, {.1F, .2F, .1F}, {.1F, -.2F, .1F}, {4, 0, 0}}};
    for (std::size_t i = 0; i < 4; ++i) {
        skinned[i].position = points[i];
        skinned[i].normal = {1, 0, 0};
        std::copy(points[i].begin(), points[i].end(), source[i].position);
        source[i].normal[0] = 1;
        if (i < 3) {
            source[i].position[0] -= .5F;
            source[i].gpuSkinning = 1;
            source[i].bones[0] = 0;
        }
    }
    const std::vector<core::SkirtVertex> selected{
        {0, 0, 1, 0, 1, 1, {}}, {1, 1, 1, 0, 1, 1, {}}, {2, 2, 1, 0, 1, 1, {}}};
    const std::vector<std::array<std::uint32_t, 4>> triangles{{0, 1, 2, 0}};
    const std::vector<std::uint32_t> adjacency{0, 0, 0}, indices{0, 1, 2};
    const std::vector<core::SkirtCapsule> capsules{
        {{0, -1, 0, 1}, {0, 1, 0, .03F}, {0, -1, 0, 1}, {0, 1, 0, .03F}, {0, 0, 0, 1}}};
    const graphics::SkirtCollisionUpload collision{selected, triangles, adjacency, capsules, 1, 32};
    GpuFixture fixture(device, 4, false);
    const auto expected = fixture.run(skinned, collision);
    graphics::PreviewBoneTransform bone;
    bone.translation[0] = .5F;
    const std::array bones{bone};
    graphics::NativeDeformUpload upload{.baseVertices = source,
                                        .bones = bones,
                                        .morphDeltas = {},
                                        .morphWeights = {},
                                        .indices = indices,
                                        .deformedVertices = {},
                                        .skirtCollision = collision};
    graphics::NativeDeformRuntime runtime;
    graphics::SkirtCollisionRuntime computeCollision;
    std::array<graphics::handles::BufferHandle, 5> inputs{};
    graphics::handles::DescriptorSetHandle set{};
    graphics::handles::BufferHandle output;
    const bool blasSupported = device.capabilities().accelerationStructure;
    if (blasSupported) {
        std::string error;
        if (!runtime.initialize(device, upload, device.nativeDeformPipeline(), device.nativeDeformDescriptorLayout(),
                                &error, 1))
            throw std::runtime_error(error);
    } else {
        // Exercise the exact Native shader on compute-only hardware. The
        // production runtime additionally requires BLAS-capable buffers.
        const std::array<std::size_t, 5> sizes{source.size() * sizeof(source[0]), sizeof(bone), 16, 16,
                                               source.size() * sizeof(graphics::NativeDeformedVertex)};
        for (std::size_t i = 0; i < inputs.size(); ++i)
            inputs[i] = device.createBufferEx({.size = sizes[i],
                                               .usage = graphics::ResourceUsage::storageRead |
                                                        graphics::ResourceUsage::storageWrite |
                                                        graphics::ResourceUsage::transferSrc,
                                               .cpuVisible = true});
        device.uploadBufferEx(inputs[0], std::as_bytes(std::span(source)), 0);
        device.uploadBufferEx(inputs[1], std::as_bytes(std::span(bones)), 0);
        std::array<graphics::DescriptorBindingEx, 5> bindings;
        for (std::uint32_t i = 0; i < 5; ++i)
            bindings[i] = {.slot = i, .buffer = inputs[i], .mipLevel = {}};
        set = device.allocateDescriptorSetEx(device.nativeDeformDescriptorLayout(), bindings);
        output = inputs[4];
    }
    device.setNativeFrameRecorderForComputeTest(
        [&](graphics::CommandList& commands,
            const graphics::RenderTargetDesc&) -> std::optional<graphics::NativeFrameOutput> {
            if (blasSupported) {
                runtime.record(commands, upload, 1);
                output = runtime.resources().deformedVertices;
            } else {
                const graphics::NativeDeformPushConstants constants{.vertexCount = 4, .boneCount = 1};
                commands.bindPipelineEx(device.nativeDeformPipeline());
                commands.bindDescriptorSetEx(set);
                commands.pushConstantsEx(std::as_bytes(std::span(&constants, 1)));
                commands.dispatch(1, 1, 1);
                computeCollision.record(device, commands, output, collision, 48, 16);
            }
            return std::nullopt;
        });
    static_cast<void>(device.renderToImage({16, 16}));
    device.waitIdle();
    device.setNativeFrameRecorder({});
    const auto bytes = device.readbackBufferEx(output, 0, 4 * sizeof(graphics::NativeDeformedVertex));
    for (std::size_t i = 0; i < 4; ++i) {
        graphics::NativeDeformedVertex actual;
        std::memcpy(&actual, bytes.data() + i * sizeof(actual), sizeof(actual));
        for (std::size_t axis = 0; axis < 3; ++axis)
            require(std::abs(actual.position[axis] - expected.positions[i][axis]) < 1e-5F,
                    "Native GPU skinning plus collision differs from host-skinned reference");
    }
    require((blasSupported ? runtime.skirtCollisionStats() : computeCollision.readStats()).penetratingVertexCount == 0,
            "Native post-skinning collision did not run");
    computeCollision.reset();
    if (set.valid())
        device.destroyDescriptorSetEx(set);
    for (auto buffer : inputs)
        if (buffer.valid())
            device.destroyBufferEx(buffer);
    std::cout << "Native post-skinning collision integration passed\n";
}

void fittedCapsuleFixture(graphics::VulkanDevice& device) {
    std::vector<core::PmxVertex> points(4);
    points[0].position = {2.8F, 1.2F, 0};
    points[1].position = {2.9F, 1.2F, .1F};
    points[2].position = {2.8F, 1.2F, -.1F};
    points[3].position = {3, 1.6F, 0};
    for (auto& point : points)
        point.normal = {0, 1, 0};
    const std::vector<core::SkirtVertex> vertices{
        {0, 0, 1, 0, 1, 1, {}}, {1, 1, 1, 0, 1, 1, {}}, {2, 2, 1, 0, 1, 1, {}}};
    const std::vector<std::array<std::uint32_t, 4>> triangles{{0, 1, 2, 0}};
    const std::vector<std::uint32_t> adjacency{0, 0, 0}, legVertices{3};
    const std::vector<std::array<std::uint32_t, 2>> legRanges{{0, 1}};
    const std::vector<core::SkirtCapsule> capsules{
        {{-1, 0, 0, 1}, {1, 0, 0, .03F}, {-1, 0, 0, 1}, {1, 0, 0, .03F}, {0, 0, 0, 1}}};
    const graphics::SkirtCollisionUpload upload{vertices, triangles, adjacency, capsules,
                                                1,        32,        legRanges, legVertices};
    GpuFixture preview(device, 4, false), native(device, 4, true);
    const auto a = preview.run(points, upload), b = native.run(points, upload);
    require(a.capsules[0].aRadius[3] > 1.6F && a.capsules[0].bMargin[0] > 2.8F,
            "fitted capsule did not cover the wider and longer rendered leg");
    require(distance(points[3].position, a.capsules[0]) <= -a.capsules[0].bMargin[3] + 1e-4F,
            "rendered leg escaped its contact proxy");
    require(a.positions[3] == points[3].position, "fitting changed the rendered leg");
    require(a.stats.penetratingVertexCount == 0 && a.stats.totalCorrectionDistance > .5F,
            "cloth outside the PMX body was not corrected against the visible leg");
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t axis = 0; axis < 3; ++axis)
            require(std::abs(a.positions[i][axis] - b.positions[i][axis]) < 1e-5F,
                    "fitted capsule collision layouts diverged");
    points[3].position[0] = 4;
    const auto next = preview.run(points, upload);
    require(std::abs(next.capsules[0].previousB[0] - a.capsules[0].bMargin[0]) < 1e-5F,
            "fitted capsule history retained the shorter PMX endpoint");
}

void modelGpuParity(graphics::VulkanDevice& device, const core::PmxModel& model, const core::AnimatedModelFrame& cpu,
                    const core::AnimatedModelFrame& gpu, const core::SkirtCollisionTopology& topology,
                    std::span<const core::SkirtCapsule> capsules, std::uint64_t epoch) {
    std::vector<std::vector<graphics::PreviewMorphDelta>> perVertex(model.vertices.size());
    for (std::size_t m = 0; m < model.morphs.size(); ++m) {
        if (model.morphs[m].type != 1)
            continue;
        for (const auto& delta : model.morphs[m].offsets) {
            if (delta.index < 0 || static_cast<std::size_t>(delta.index) >= perVertex.size())
                continue;
            graphics::PreviewMorphDelta value;
            std::copy_n(delta.vector3.begin(), 3, value.delta);
            value.morphIndex = static_cast<std::uint32_t>(m);
            perVertex[static_cast<std::size_t>(delta.index)].push_back(value);
        }
    }
    std::vector<graphics::PreviewVertex> source;
    source.reserve(gpu.vertices.size());
    std::vector<graphics::PreviewMorphDelta> deltas;
    for (std::size_t i = 0; i < gpu.vertices.size(); ++i) {
        source.push_back(app::makePreviewVertex(
            gpu.vertices[i], gpu.bones.size(), 0, {},
            {static_cast<std::uint32_t>(deltas.size()), static_cast<std::uint32_t>(perVertex[i].size())}, true));
        deltas.insert(deltas.end(), perVertex[i].begin(), perVertex[i].end());
    }
    std::vector<graphics::PreviewBoneTransform> bones;
    bones.reserve(gpu.bones.size());
    for (const auto& bone : gpu.bones)
        bones.push_back(app::makePreviewBone(bone, {}));
    device.uploadPreviewMesh(source, model.indices);
    device.updatePreviewBones(bones);
    device.uploadPreviewMorphDeltas(deltas);
    device.updatePreviewMorphWeights(gpu.morphWeights);
    device.updatePreviewSkirtCollision({}, {}, {}, {}, 0);
    static_cast<void>(device.renderToImage({16, 16}));
    const auto raw = device.readbackPreviewDeformedVertices();
    float legDifference = 0;
    for (const auto vertex : topology.legVertices)
        for (std::size_t axis = 0; axis < 3; ++axis)
            legDifference =
                std::max(legDifference, std::abs(raw[vertex].position[axis] - cpu.vertices[vertex].position[axis]));
    if (legDifference >= 5e-4F)
        std::cerr << "Raw CPU/Preview leg maximum difference: " << legDifference << '\n';
    for (const auto v : topology.vertices)
        for (std::size_t axis = 0; axis < 3; ++axis)
            require(std::abs(raw[v.vertex].position[axis] - cpu.vertices[v.vertex].position[axis]) < 5e-4F,
                    "YYB CPU/Preview GPU skinning diverged");
    const graphics::SkirtCollisionUpload collision{
        topology.vertices,  topology.triangles,  topology.adjacency, capsules, epoch, 32,
        topology.legRanges, topology.legVertices};
    GpuFixture cpuReference(device, model.vertices.size(), false);
    const auto expected = cpuReference.run(cpu.vertices, collision);
    device.updatePreviewSkirtCollision(topology.vertices, topology.triangles, topology.adjacency, capsules, epoch,
                                       topology.legRanges, topology.legVertices);
    static_cast<void>(device.renderToImage({16, 16}));
    const auto preview = device.readbackPreviewDeformedVertices();
    float maximumDifference = 0;
    for (const auto v : topology.vertices)
        for (std::size_t axis = 0; axis < 3; ++axis)
            maximumDifference = std::max(
                maximumDifference, std::abs(preview[v.vertex].position[axis] - expected.positions[v.vertex][axis]));
    if (maximumDifference >= 2e-3F) {
        std::cerr << "Corrected CPU/Preview GPU maximum difference: " << maximumDifference << '\n';
        const auto actualCapsules = device.readbackPreviewSkirtCapsules();
        for (std::size_t c = 0; c < actualCapsules.size(); ++c) {
            std::cerr << "Capsule " << c << " CPU/GPU radius " << expected.capsules[c].aRadius[3] << '/'
                      << actualCapsules[c].aRadius[3] << " endpoints ";
            for (std::size_t axis = 0; axis < 3; ++axis)
                std::cerr << expected.capsules[c].aRadius[axis] - actualCapsules[c].aRadius[axis] << ','
                          << expected.capsules[c].bMargin[axis] - actualCapsules[c].bMargin[axis] << ' ';
            std::cerr << '\n';
        }
    }
    require(maximumDifference < 2e-3F, "YYB corrected CPU/Preview GPU skinning diverged");
    device.updatePreviewSkirtCollision({}, {}, {}, {}, 0);
    if (device.nativeDeformPipeline().valid()) {
        graphics::SkirtCollisionRuntime correction;
        std::array<graphics::handles::BufferHandle, 5> buffers{};
        const std::array sizes{source.size() * sizeof(source[0]), bones.size() * sizeof(bones[0]),
                               deltas.size() * sizeof(deltas[0]), gpu.morphWeights.size() * sizeof(float),
                               source.size() * sizeof(graphics::NativeDeformedVertex)};
        for (std::size_t i = 0; i < 5; ++i)
            buffers[i] = device.createBufferEx({.size = std::max<std::size_t>(sizes[i], 16),
                                                .usage = graphics::ResourceUsage::storageRead |
                                                         graphics::ResourceUsage::storageWrite |
                                                         graphics::ResourceUsage::transferSrc,
                                                .cpuVisible = true});
        device.uploadBufferEx(buffers[0], std::as_bytes(std::span(source)), 0);
        device.uploadBufferEx(buffers[1], std::as_bytes(std::span(bones)), 0);
        device.uploadBufferEx(buffers[2], std::as_bytes(std::span(deltas)), 0);
        device.uploadBufferEx(buffers[3], std::as_bytes(std::span(gpu.morphWeights)), 0);
        std::array<graphics::DescriptorBindingEx, 5> bindings;
        for (std::uint32_t i = 0; i < 5; ++i)
            bindings[i] = {.slot = i, .buffer = buffers[i], .mipLevel = {}};
        const auto set = device.allocateDescriptorSetEx(device.nativeDeformDescriptorLayout(), bindings);
        device.setNativeFrameRecorderForComputeTest(
            [&](graphics::CommandList& commands,
                const graphics::RenderTargetDesc&) -> std::optional<graphics::NativeFrameOutput> {
                const graphics::NativeDeformPushConstants constants{
                    .vertexCount = static_cast<std::uint32_t>(source.size()),
                    .boneCount = static_cast<std::uint32_t>(bones.size()),
                    .morphDeltaCount = static_cast<std::uint32_t>(deltas.size()),
                    .morphCount = static_cast<std::uint32_t>(gpu.morphWeights.size())};
                commands.bindPipelineEx(device.nativeDeformPipeline());
                commands.bindDescriptorSetEx(set);
                commands.pushConstantsEx(std::as_bytes(std::span(&constants, 1)));
                commands.dispatch((constants.vertexCount + 63) / 64, 1, 1);
                correction.record(device, commands, buffers[4], collision, 48, 16);
                return std::nullopt;
            });
        static_cast<void>(device.renderToImage({16, 16}));
        device.waitIdle();
        device.setNativeFrameRecorder({});
        const auto bytes = device.readbackBufferEx(buffers[4], 0, sizes[4]);
        for (const auto v : topology.vertices) {
            graphics::NativeDeformedVertex native;
            std::memcpy(&native, bytes.data() + v.vertex * sizeof(native), sizeof(native));
            for (std::size_t axis = 0; axis < 3; ++axis)
                require(std::abs(native.position[axis] - preview[v.vertex].position[axis]) < 2e-3F,
                        "YYB Native/Preview GPU skinning plus collision diverged");
        }
        correction.reset();
        device.destroyDescriptorSetEx(set);
        for (const auto buffer : buffers)
            device.destroyBufferEx(buffer);
    }
    std::cout
        << "YYB CPU/GPU and Preview/Native skinning plus collision parity passed, maximum CPU/GPU position difference="
        << maximumDifference << '\n';
}

void modelRegression(graphics::VulkanDevice& device, const char* modelPath, const char* motionPath,
                     const char* csvPath) {
    const auto* iterationSetting = std::getenv("DAYO_SKIRT_ITERATIONS");
    const auto* firstFrameSetting = std::getenv("DAYO_SKIRT_FIRST_FRAME");
    const auto* lastFrameSetting = std::getenv("DAYO_SKIRT_LAST_FRAME");
    const auto iterations = iterationSetting ? static_cast<std::uint32_t>(std::stoi(iterationSetting)) : 32U;
    const int firstFrame = firstFrameSetting ? std::stoi(firstFrameSetting) : 0;
    const int lastFrame = lastFrameSetting ? std::stoi(lastFrameSetting) : 348;
    const auto model = core::loadPmxModel(modelPath);
    const auto motion = core::loadVmd(motionPath);
    const auto topology = core::identifySkirtCollision(model);
    require(topology.vertices.size() == 895 && topology.triangles.size() == 1192 && topology.skirtBodies.size() == 16 &&
                topology.legBodies.size() == 4,
            "YYB model classification differs from audited topology");
    mmd::PhysicsSettings settings;
    settings.adaptiveKinematicBodies = topology.legBodies;
    settings.solverIterations = 25;
    core::MmdPhysics physics(model, settings);
    core::MmdAnimator animator(model);
    animator.setPhysics(&physics);
    animator.setMotion(&motion);
    core::MmdPhysics repeatedPhysics(model, settings);
    core::MmdAnimator repeatedAnimator(model);
    repeatedAnimator.setPhysics(&repeatedPhysics);
    repeatedAnimator.setMotion(&motion);
    core::MmdPhysics gpuPhysics(model, settings);
    core::MmdAnimator gpuAnimator(model);
    gpuAnimator.setPhysics(&gpuPhysics);
    gpuAnimator.setMotion(&motion);
    GpuFixture preview(device, model.vertices.size(), false), native(device, model.vertices.size(), true),
        diagnostic(device, model.vertices.size(), false);
    std::ofstream csv(csvPath);
    csv << "frame,rigid_contacts,rigid_depth,raw_vertices,raw_triangles,raw_depth,vertices,triangles,depth,correction,"
           "substeps\n";
    std::ofstream meshCsv(std::string(csvPath) + ".mesh.csv");
    meshCsv << "frame,vertex,mobility,raw_x,raw_y,raw_z,x,y,z\n";
    std::ofstream capsuleCsv(std::string(csvPath) + ".capsules.csv");
    capsuleCsv << "frame,capsule,ax,ay,az,bx,by,bz,radius,margin\n";
    std::ofstream triangleCsv(std::string(csvPath) + ".triangles.csv");
    triangleCsv << "a,b,c\n";
    for (auto tri : topology.triangles)
        triangleCsv << topology.vertices[tri[0]].vertex << ',' << topology.vertices[tri[1]].vertex << ','
                    << topology.vertices[tri[2]].vertex << '\n';
    std::vector<mmd::PhysicsTransform> previous;
    float worstBefore = 0, worstAfter = 0;
    double correction = 0;
    std::uint64_t epoch = 1;
    std::vector<core::PmxVertex> finalVertices;
    std::vector<core::SkirtCapsule> finalCapsules;
    const auto evaluate = [&](int frame, float dt) {
        auto animated = animator.evaluate(static_cast<float>(frame), dt, false);
        const auto repeated = repeatedAnimator.evaluate(static_cast<float>(frame), dt, false);
        const auto gpu = gpuAnimator.evaluate(static_cast<float>(frame), dt, true);
        for (const auto vertex : topology.vertices)
            for (std::size_t axis = 0; axis < 3; ++axis)
                require(std::abs(animated.vertices[vertex.vertex].position[axis] -
                                 repeated.vertices[vertex.vertex].position[axis]) < 1e-5F,
                        "fixed-rate playback/export evaluation is not deterministic");
        auto capsules = core::makeSkirtCapsules(model, topology, physics, animated.bones, {}, previous);
        finalVertices = animated.vertices;
        finalCapsules = capsules;
        previous.clear();
        for (auto body : topology.legBodies)
            previous.push_back(physics.bodyTransform(body));
        if (frame < firstFrame && dt > 0)
            return;
        graphics::SkirtCollisionUpload upload{
            topology.vertices, topology.triangles, topology.adjacency,  capsules, epoch,
            iterations,        topology.legRanges, topology.legVertices};
        // Measure before correction with an independent history reset.
        auto rawUpload = upload;
        rawUpload.iterations = 0;
        const auto raw = diagnostic.run(animated.vertices, rawUpload);
        const auto a = preview.run(animated.vertices, upload), b = native.run(animated.vertices, upload);
        for (std::size_t leg = 0; leg < topology.legRanges.size(); ++leg) {
            const auto range = topology.legRanges[leg];
            for (std::size_t i = 0; i < range[1]; ++i)
                require(distance(animated.vertices[topology.legVertices[range[0] + i]].position, a.capsules[leg]) <=
                            -a.capsules[leg].bMargin[3] * 1.25F + 1e-4F,
                        "YYB rendered leg surface escaped its fitted collision capsule");
        }
        for (const auto vertex : topology.vertices)
            for (std::size_t axis = 0; axis < 3; ++axis)
                require(std::abs(a.positions[vertex.vertex][axis] - b.positions[vertex.vertex][axis]) < 1e-5F,
                        "YYB Preview/Native corrected positions diverged");
        if (frame == 123 || a.stats.maxMeshPenetration > .05F) {
            for (const auto v : topology.vertices) {
                meshCsv << frame << ',' << v.vertex << ',' << v.mobility;
                for (float x : animated.vertices[v.vertex].position)
                    meshCsv << ',' << x;
                for (float x : a.positions[v.vertex])
                    meshCsv << ',' << x;
                meshCsv << '\n';
            }
            for (std::size_t c = 0; c < capsules.size(); ++c) {
                capsuleCsv << frame << ',' << c;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    capsuleCsv << ',' << a.capsules[c].aRadius[axis];
                for (std::size_t axis = 0; axis < 3; ++axis)
                    capsuleCsv << ',' << a.capsules[c].bMargin[axis];
                capsuleCsv << ',' << a.capsules[c].aRadius[3] << ',' << a.capsules[c].bMargin[3] << '\n';
            }
        }
        meshCsv.flush();
        capsuleCsv.flush();
        if (iterations == 32 && (frame == 0 || frame == 115 || frame == 120 || frame == 127 || frame == 130 ||
                                 frame == 140 || frame == 150 || frame == 165 || frame == 348))
            modelGpuParity(device, model, animated, gpu, topology, capsules,
                           epoch * 1000 + static_cast<std::uint64_t>(frame));
        const auto rigid = core::skirtRigidStats(topology, physics);
        csv << frame << ',' << rigid.rigidContactCount << ',' << rigid.maxRigidPenetration << ','
            << raw.stats.penetratingVertexCount << ',' << raw.stats.penetratingTriangleCount << ','
            << raw.stats.maxMeshPenetration << ',' << a.stats.penetratingVertexCount << ','
            << a.stats.penetratingTriangleCount << ',' << a.stats.maxMeshPenetration << ','
            << a.stats.totalCorrectionDistance << ',' << physics.stepStats().substeps << '\n';
        worstBefore = std::max(worstBefore, raw.stats.maxMeshPenetration);
        worstAfter = std::max(worstAfter, a.stats.maxMeshPenetration);
        correction += a.stats.totalCorrectionDistance;
        if (frame % 30 == 0)
            std::cout << "YYB frame=" << frame << " raw_depth=" << raw.stats.maxMeshPenetration
                      << " corrected_depth=" << a.stats.maxMeshPenetration << "\n";
    };
    for (int frame = 0; frame <= lastFrame; ++frame)
        evaluate(frame, 1.0F / 30);
    previous.clear();
    ++epoch;
    evaluate(0, 0);
    previous.clear();
    ++epoch;
    evaluate(127, 0);
    std::cout << "YYB UNDEAD: worst raw depth=" << worstBefore << " corrected=" << worstAfter
              << " total correction=" << correction << '\n';
    require(worstAfter < .031F && worstAfter < worstBefore * .1F, "YYB correction left deep mesh penetration");
    // The existing frame timestamps include the same tiny render target in
    // both cases. Warm each frame slot, then report the median difference.
    GpuFixture timing(device, model.vertices.size(), false);
    std::ofstream timingCsv(std::string(csvPath) + ".timing.csv");
    timingCsv << "enabled,sample,gpu_ns\n";
    std::array<double, 2> medians{};
    for (std::size_t enabled = 0; enabled < 2; ++enabled) {
        std::vector<std::uint64_t> samples;
        for (int sample = 0; sample < 30; ++sample) {
            graphics::SkirtCollisionUpload timed;
            if (enabled)
                timed = {topology.vertices,  topology.triangles,  topology.adjacency, finalCapsules, epoch, 32,
                         topology.legRanges, topology.legVertices};
            const auto result = timing.run(finalVertices, timed);
            if (sample >= 6 && result.gpuNanoseconds != 0) {
                samples.push_back(result.gpuNanoseconds);
                timingCsv << enabled << ',' << sample << ',' << result.gpuNanoseconds << '\n';
            }
        }
        if (!samples.empty()) {
            std::sort(samples.begin(), samples.end());
            medians[enabled] = static_cast<double>(samples[samples.size() / 2]) / 1e6;
        }
    }
    std::cout << "YYB GPU median: disabled=" << medians[0] << " ms enabled=" << medians[1]
              << " ms delta=" << medians[1] - medians[0] << " ms\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        topologyFixtures();
        const auto window = dayo::platform::createWindow({"skirt collision regression", 64, 64, true});
        dayo::graphics::VulkanOptions options;
        options.asyncCompute = argc == 2 && std::string_view(argv[1]) == "--async-compute";
        dayo::graphics::VulkanDevice device(*window, true, options);
        gpuFixtures(device);
        fittedCapsuleFixture(device);
        previewIntegration(device);
        nativeSkinningIntegration(device);
        if (argc == 4)
            modelRegression(device, argv[1], argv[2], argv[3]);
        require(device.validationErrorCount() == 0, "Vulkan validation reported collision hazards");
        std::cout << "skirt collision tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
