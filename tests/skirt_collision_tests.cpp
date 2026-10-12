#include "app/preview_conversion.hpp"
#include "core/skirt_collision.hpp"
#include "graphics/preview_gpu_scene.hpp"
#include "graphics/skirt_collision.hpp"
#include "graphics/subayai_deform.hpp"
#include "graphics/vulkan/vulkan_device.hpp"
#include "platform/window.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
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
    model.rigidBodies = {cloth, leg};
    auto topology = core::identifySkirtCollision(model);
    require(topology.vertices.size() == 3 && topology.triangles.size() == 1 && topology.legBodies.size() == 1,
            "skirt extraction includes unrelated material or loses leg");
    require(std::ranges::none_of(topology.vertices, [](auto v) { return v.vertex == 3; }), "unrelated vertex selected");
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
    points[1].position = {-1, 1.7320508F, 0};
    points[2].position = {-1, -1.7320508F, 0};
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
    }
    const std::vector<core::SkirtVertex> selected{
        {0, 0, 1, 0, 1, 1, {}}, {1, 1, 1, 0, 1, 1, {}}, {2, 2, 1, 0, 1, 1, {}}};
    const std::vector<std::array<std::uint32_t, 4>> triangles{{0, 1, 2, 0}};
    const std::vector<std::uint32_t> adjacency{0, 0, 0}, indices{0, 1, 2};
    const std::vector<core::SkirtCapsule> capsules{
        {{0, -1, 0, 1}, {0, 1, 0, .03F}, {0, -1, 0, 1}, {0, 1, 0, .03F}, {0, 0, 0, 1}}};
    GpuFixture fixture(device, 4, true);
    const auto expected = fixture.run(reference, {selected, triangles, adjacency, capsules, 1, 6});
    device.uploadPreviewMesh(vertices, indices);
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

void modelRegression(graphics::VulkanDevice& device, const char* modelPath, const char* motionPath,
                     const char* csvPath) {
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
    GpuFixture preview(device, model.vertices.size(), false), native(device, model.vertices.size(), true),
        diagnostic(device, model.vertices.size(), false);
    std::ofstream csv(csvPath);
    csv << "frame,rigid_contacts,rigid_depth,raw_vertices,raw_triangles,raw_depth,vertices,triangles,depth,correction,"
           "substeps\n";
    std::vector<mmd::PhysicsTransform> previous;
    float worstBefore = 0, worstAfter = 0;
    double correction = 0;
    std::uint64_t epoch = 1;
    const auto evaluate = [&](int frame, float dt) {
        auto animated = animator.evaluate(static_cast<float>(frame), dt, false);
        auto capsules = core::makeSkirtCapsules(model, topology, physics, animated.bones, {}, previous);
        previous.clear();
        for (auto body : topology.legBodies)
            previous.push_back(physics.bodyTransform(body));
        graphics::SkirtCollisionUpload upload{
            topology.vertices, topology.triangles, topology.adjacency, capsules, epoch, 6};
        // Measure before correction with an independent history reset.
        auto rawUpload = upload;
        rawUpload.iterations = 0;
        const auto raw = diagnostic.run(animated.vertices, rawUpload);
        const auto a = preview.run(animated.vertices, upload), b = native.run(animated.vertices, upload);
        for (const auto vertex : topology.vertices)
            for (std::size_t axis = 0; axis < 3; ++axis)
                require(std::abs(a.positions[vertex.vertex][axis] - b.positions[vertex.vertex][axis]) < 1e-5F,
                        "YYB Preview/Native corrected positions diverged");
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
    for (int frame = 0; frame <= 348; ++frame)
        evaluate(frame, 1.0F / 30);
    previous.clear();
    ++epoch;
    evaluate(0, 0);
    previous.clear();
    ++epoch;
    evaluate(127, 0);
    std::cout << "YYB UNDEAD: worst raw depth=" << worstBefore << " corrected=" << worstAfter
              << " total correction=" << correction << '\n';
    require(worstAfter < worstBefore, "YYB correction did not improve maximum penetration");
}
} // namespace
int main(int argc, char** argv) {
    try {
        topologyFixtures();
        const auto window = dayo::platform::createWindow({"skirt collision regression", 64, 64, true});
        dayo::graphics::VulkanDevice device(*window, true);
        gpuFixtures(device);
        previewIntegration(device);
        if (argc == 4)
            modelRegression(device, argv[1], argv[2], argv[3]);
        require(device.validationErrorCount() == 0, "Vulkan validation reported collision hazards");
        std::cout << "skirt collision tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
