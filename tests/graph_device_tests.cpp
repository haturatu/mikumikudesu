#include "core/image.hpp"
#include "graphics/device.hpp"
#include "graphics/handles.hpp"
#include "graphics/render_graph.hpp"
#include "graphics/resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

using dayo::graphics::BufferDesc;
using dayo::graphics::BufferResourceDesc;
using dayo::graphics::Device;
using dayo::graphics::DeviceCapabilities;
using dayo::graphics::GraphicsConvention;
using dayo::graphics::PreviewBoneTransform;
using dayo::graphics::PreviewDraw;
using dayo::graphics::PreviewMaterial;
using dayo::graphics::PreviewMorphDelta;
using dayo::graphics::PreviewScene;
using dayo::graphics::PreviewTexture;
using dayo::graphics::PreviewVertex;
using dayo::graphics::RendererKind;
using dayo::graphics::RenderGraph;
using dayo::graphics::RenderTargetDesc;
using dayo::graphics::ResourceLifetime;
using dayo::graphics::ResourceRegistry;
using dayo::graphics::ResourceUsage;
using dayo::graphics::TextureDesc;
using dayo::graphics::TextureDimension;
using dayo::graphics::TextureResourceDesc;

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

// Minimal mock: implements only the legacy Preview contract. New typed
// virtuals keep the base logic_error defaults, proving Preview backends stay
// buildable while mocks remain usable for graph/registry unit tests.
class MockDevice final : public Device {
  public:
    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] const GraphicsConvention& convention() const noexcept override {
        return convention_;
    }
    [[nodiscard]] RendererKind activeRenderer() const noexcept override {
        return RendererKind::preview;
    }
    void selectRenderer(RendererKind) override {}
    void resize() override {}
    void beginUiFrame() override {}
    void renderFrame() override {}
    void waitIdle() override {}
    void uploadPreviewMesh(std::span<const PreviewVertex>, std::span<const std::uint32_t>) override {}
    void updatePreviewVertices(std::span<const PreviewVertex>) override {}
    void updatePreviewBones(std::span<const PreviewBoneTransform>) override {}
    void uploadPreviewMorphDeltas(std::span<const PreviewMorphDelta>) override {}
    void updatePreviewMorphWeights(std::span<const float>) override {}
    void updatePreviewMaterials(std::span<const PreviewMaterial>) override {}
    void updatePreviewDraws(std::span<const PreviewDraw>) override {}
    void uploadPreviewTextures(std::span<const PreviewTexture>) override {}
    void uploadPreviewBackground(std::span<const PreviewTexture>) override {}
    void clearPreviewResources() override {}
    void updatePreviewScene(const PreviewScene&) override {}
    [[nodiscard]] dayo::graphics::BufferHandle createBuffer(const BufferDesc&) override {
        return nextHandle_++;
    }
    [[nodiscard]] dayo::graphics::TextureHandle createTexture(const TextureDesc&) override {
        return nextHandle_++;
    }

  private:
    DeviceCapabilities capabilities_;
    GraphicsConvention convention_;
    dayo::graphics::BufferHandle nextHandle_{1};
};

TextureResourceDesc makeTransientDesc() {
    TextureResourceDesc desc;
    desc.dimension = TextureDimension::d2;
    desc.extent = {64, 64, 1};
    desc.format = dayo::graphics::PixelFormat::rgba8Unorm;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.usage = ResourceUsage::sampledRead;
    desc.lifetime = ResourceLifetime::transient;
    return desc;
}

TextureResourceDesc makePersistentDesc() {
    TextureResourceDesc desc = makeTransientDesc();
    desc.lifetime = ResourceLifetime::persistent;
    return desc;
}

} // namespace

int main() {
    bool ok = true;

    // ---- barrier generation: write -> read inserts a transition + dependency ----
    {
        RenderGraph graph;
        const auto color = graph.createResource("color");
        static_cast<void>(graph.addPass("opaque", {{color, ResourceUsage::colorAttachment}}));
        static_cast<void>(graph.addPass("post", {{color, ResourceUsage::sampledRead}}));
        const auto plan = graph.compile();
        ok &= check(plan.size() == 2, "typed graph keeps pass count");
        ok &= check(plan[1].dependencies.size() == 1 && plan[1].dependencies.front() == 0,
                    "typed graph tracks write->read dependency");
        ok &= check(plan[1].barriers.size() == 1, "typed graph emits write->read barrier");
        if (plan.size() == 2 && !plan[1].barriers.empty()) {
            ok &= check(plan[1].barriers.front().before == ResourceUsage::colorAttachment &&
                            plan[1].barriers.front().after == ResourceUsage::sampledRead,
                        "typed barrier carries before/after usage");
        }
    }
    // ---- barrier generation: no transition when usage is unchanged ----
    {
        RenderGraph graph;
        const auto texture = graph.createResource("texture");
        static_cast<void>(graph.addPass("sample-a", {{texture, ResourceUsage::sampledRead}}));
        static_cast<void>(graph.addPass("sample-b", {{texture, ResourceUsage::sampledRead}}));
        const auto plan = graph.compile();
        ok &= check(plan.size() == 2 && plan[1].barriers.empty(), "typed graph skips redundant barriers");
        ok &= check(plan[1].dependencies.empty(), "typed graph skips read->read dependency");
    }
    // ---- barrier generation: representative typed transitions ----
    {
        RenderGraph graph;
        const auto depth = graph.createResource("depth");
        const auto upload = graph.createResource("upload");
        const auto volume = graph.createResource("volume");
        const auto frame = graph.createResource("frame");
        static_cast<void>(graph.addPass("depth", {{depth, ResourceUsage::depthWrite}}));
        static_cast<void>(graph.addPass("depth-sample", {{depth, ResourceUsage::depthRead}}));
        static_cast<void>(graph.addPass("upload", {{upload, ResourceUsage::transferDst}}));
        static_cast<void>(graph.addPass("copy", {{upload, ResourceUsage::transferSrc}}));
        static_cast<void>(graph.addPass("write3d", {{volume, ResourceUsage::storageWrite}}));
        static_cast<void>(graph.addPass("read3d", {{volume, ResourceUsage::storageRead}}));
        static_cast<void>(graph.addPass("render", {{frame, ResourceUsage::colorAttachment}}));
        static_cast<void>(graph.addPass("present", {{frame, ResourceUsage::present}}));
        const auto plan = graph.compile();
        ok &= check(plan.size() == 8, "typed graph covers depth/transfer/storage/present passes");
        if (plan.size() == 8) {
            ok &= check(plan[1].barriers.size() == 1 && plan[1].barriers.front().before == ResourceUsage::depthWrite &&
                            plan[1].barriers.front().after == ResourceUsage::depthRead,
                        "depthWrite->depthRead barrier");
            ok &= check(plan[3].barriers.size() == 1 && plan[3].barriers.front().before == ResourceUsage::transferDst &&
                            plan[3].barriers.front().after == ResourceUsage::transferSrc,
                        "transferDst->transferSrc barrier");
            ok &=
                check(plan[5].barriers.size() == 1 && plan[5].barriers.front().before == ResourceUsage::storageWrite &&
                          plan[5].barriers.front().after == ResourceUsage::storageRead,
                      "storageWrite->storageRead barrier");
            ok &= check(plan[7].barriers.size() == 1 &&
                            plan[7].barriers.front().before == ResourceUsage::colorAttachment &&
                            plan[7].barriers.front().after == ResourceUsage::present,
                        "colorAttachment->present barrier");
        }
    }
    // ---- barrier generation: storage/UAV + AS/indirect/host/external coverage ----
    {
        RenderGraph graph;
        const auto uav = graph.createResource("uav");
        const auto geometry = graph.createResource("geometry");
        const auto counter = graph.createResource("counter");
        static_cast<void>(graph.addPass("uav-write", {{uav, ResourceUsage::storageReadWrite}}));
        static_cast<void>(graph.addPass("uav-read", {{uav, ResourceUsage::sampledRead}}));
        static_cast<void>(graph.addPass("as-build", {{geometry, ResourceUsage::asBuildWrite}}));
        static_cast<void>(graph.addPass("as-trace", {{geometry, ResourceUsage::asBuildRead}}));
        static_cast<void>(graph.addPass("produce", {{counter, ResourceUsage::externalWrite}}));
        static_cast<void>(graph.addPass("consume", {{counter, ResourceUsage::externalRead}}));
        const auto plan = graph.compile();
        ok &= check(plan.size() == 6, "extended usage passes compile");
        if (ok) {
            ok &= check(!plan[1].barriers.empty() && plan[1].barriers.front().after == ResourceUsage::sampledRead,
                        "storageReadWrite->sampledRead barrier");
            ok &= check(!plan[1].dependencies.empty(), "storage write tracks dependency");
            ok &= check(!plan[3].barriers.empty() && plan[3].barriers.front().before == ResourceUsage::asBuildWrite &&
                            plan[3].barriers.front().after == ResourceUsage::asBuildRead,
                        "asBuildWrite->asBuildRead barrier");
            ok &= check(!plan[5].barriers.empty() && plan[5].barriers.front().before == ResourceUsage::externalWrite &&
                            plan[5].barriers.front().after == ResourceUsage::externalRead,
                        "externalWrite->externalRead barrier");
        }
        // Same-pass read+write must not create a self dependency.
        RenderGraph selfGraph;
        const auto self = selfGraph.createResource("self");
        static_cast<void>(
            selfGraph.addPass("read-write", {{self, ResourceUsage::sampledRead}, {self, ResourceUsage::storageWrite}}));
        const auto selfPlan = selfGraph.compile();
        ok &= check(selfPlan.size() == 1 && selfPlan.front().dependencies.empty(),
                    "typed graph ignores same-pass self dependencies");
    }

    // ---- lifetime calculation ----
    {
        RenderGraph graph;
        const auto span = graph.createResource("span");
        const auto single = graph.createResource("single");
        const auto unused = graph.createResource("unused");
        static_cast<void>(graph.addPass("p0", {{span, ResourceUsage::colorAttachment}}));
        static_cast<void>(graph.addPass("p1", {{single, ResourceUsage::sampledRead}}));
        static_cast<void>(graph.addPass("p2", {{span, ResourceUsage::sampledRead}}));
        const auto lifetimes = graph.lifetimes();
        ok &= check(lifetimes.size() == 3, "typed lifetimes cover all resources");
        if (lifetimes.size() == 3) {
            ok &= check(lifetimes[span].firstPass == 0 && lifetimes[span].lastPass == 2,
                        "typed lifetime spans first to last use");
            ok &=
                check(lifetimes[single].firstPass == 1 && lifetimes[single].lastPass == 1, "single-use typed lifetime");
            ok &= check(lifetimes[unused].firstPass == RenderGraph::invalidPass &&
                            lifetimes[unused].lastPass == RenderGraph::invalidPass,
                        "unused typed resource stays invalid");
        }
    }

    // ---- alias判定: transient may alias, persistent history may not ----
    {
        const auto transientA = makeTransientDesc();
        const auto transientB = makeTransientDesc();
        const auto previousFrame = makePersistentDesc();
        const auto bdptAccumulation = makePersistentDesc();
        ok &= check(dayo::graphics::canAlias(transientA, transientB), "transient resources may alias");
        ok &=
            check(!dayo::graphics::canAlias(transientA, previousFrame), "transient cannot alias PreviousFrame history");
        ok &= check(!dayo::graphics::canAlias(previousFrame, bdptAccumulation),
                    "persistent BDPT accumulation cannot alias");
        ok &= check(dayo::graphics::isAliasingAllowed(ResourceLifetime::transient) &&
                        !dayo::graphics::isAliasingAllowed(ResourceLifetime::persistent),
                    "aliasing gate follows lifetime");
        ok &= check(dayo::graphics::mustPreserveForHistory(ResourceLifetime::persistent) &&
                        !dayo::graphics::mustPreserveForHistory(ResourceLifetime::transient),
                    "history preservation follows lifetime");

        RenderGraph graph;
        const auto scratchA = graph.createResource("scratch-a", transientA);
        const auto scratchB = graph.createResource("scratch-b", transientB);
        const auto history = graph.createResource("previous-frame", previousFrame);
        const auto particle = graph.createResource("particle-history", makePersistentDesc());
        ok &= check(graph.canAlias(scratchA, scratchB), "graph scratch resources may alias");
        ok &= check(!graph.canAlias(scratchA, history), "graph history resource is not aliasable");
        ok &= check(!graph.canAlias(history, particle), "graph persistent pair is not aliasable");

        ResourceRegistry registry;
        const auto regTransientA = registry.createTexture(transientA);
        const auto regTransientB = registry.createTexture(transientB);
        const auto regHistory = registry.createTexture(previousFrame);
        ok &= check(registry.canAliasTextures(regTransientA, regTransientB), "registry transient pair may alias");
        ok &= check(!registry.canAliasTextures(regTransientA, regHistory), "registry history pair may not alias");
        // 3D/mip/array descriptors round-trip through the registry.
        TextureResourceDesc volume = makeTransientDesc();
        volume.dimension = TextureDimension::d3;
        volume.extent = {32, 32, 16};
        volume.mipLevels = 4;
        volume.arrayLayers = 1;
        volume.usage = ResourceUsage::storageReadWrite;
        const auto regVolume = registry.createTexture(volume);
        const auto* found = registry.findTexture(regVolume);
        ok &= check(found != nullptr && found->dimension == TextureDimension::d3 && found->extent.depth == 16 &&
                        found->mipLevels == 4 && found->usage == ResourceUsage::storageReadWrite,
                    "3D/mip typed descriptor round trip");
        TextureResourceDesc array = makeTransientDesc();
        array.arrayLayers = 6;
        array.mipLevels = 3;
        const auto regArray = registry.createTexture(array);
        const auto* foundArray = registry.findTexture(regArray);
        ok &= check(foundArray != nullptr && foundArray->arrayLayers == 6 && foundArray->mipLevels == 3,
                    "array/mip typed descriptor round trip");
        ok &= check(dayo::graphics::isValidTextureDesc(volume) && dayo::graphics::isValidTextureDesc(array),
                    "typed texture validation accepts 3D/array/mip");
    }

    // ---- stale検出: generation mismatch ----
    {
        dayo::graphics::handles::TexturePool pool;
        const auto first = pool.create();
        ok &= check(pool.isAlive(first), "fresh typed handle is alive");
        const auto staleCopy = first;
        ok &= check(pool.destroy(first), "typed destroy succeeds once");
        ok &= check(!pool.isAlive(staleCopy), "destroyed handle is stale");
        ok &= check(!pool.destroy(staleCopy), "double destroy reports stale");
        const auto second = pool.create();
        ok &= check(pool.isAlive(second), "recycled slot is alive");
        ok &= check(!pool.isAlive(staleCopy), "old generation stays stale after recycle");
        ok &= check(second.index == first.index && second.generation != first.generation,
                    "recycle reuses index with a new generation");
        // RAII guard releases the slot exactly once.
        {
            dayo::graphics::handles::TexturePool scopedPool;
            dayo::graphics::handles::ScopedHandle<dayo::graphics::handles::TextureHandle> guard(&scopedPool,
                                                                                                scopedPool.create());
            ok &= check(static_cast<bool>(guard) && guard.alive(), "scoped typed handle is alive");
        }
        // Registry-level stale detection.
        ResourceRegistry registry;
        const auto texture = registry.createTexture(makeTransientDesc());
        const auto textureStale = texture;
        ok &= check(registry.isTextureAlive(texture), "registry handle is alive");
        ok &= check(registry.destroyTexture(texture), "registry destroy succeeds");
        ok &= check(!registry.isTextureAlive(textureStale), "registry handle is stale after destroy");
        ok &= check(registry.findTexture(textureStale) == nullptr, "registry lookup rejects stale handles");
        ok &= check(!registry.canAliasTextures(textureStale, textureStale), "registry alias rejects stale handles");
        BufferResourceDesc bufferDesc;
        bufferDesc.size = 256;
        bufferDesc.usage = ResourceUsage::uniformRead;
        bufferDesc.lifetime = ResourceLifetime::transient;
        const auto buffer = registry.createBuffer(bufferDesc);
        const auto bufferStale = buffer;
        ok &= check(registry.destroyBuffer(buffer) && !registry.isBufferAlive(bufferStale),
                    "registry buffer stale detection");
    }

    // ---- Device拡張: mock stays usable, new virtuals fail explicitly ----
    {
        MockDevice device;
        bool textureThrew = false;
        try {
            static_cast<void>(device.createTextureEx(makeTransientDesc()));
        } catch (const std::logic_error&) {
            textureThrew = true;
        }
        ok &= check(textureThrew, "mock typed texture creation reports unimplemented");

        bool bufferThrew = false;
        try {
            BufferResourceDesc desc;
            desc.size = 64;
            static_cast<void>(device.createBufferEx(desc));
        } catch (const std::logic_error&) {
            bufferThrew = true;
        }
        ok &= check(bufferThrew, "mock typed buffer creation reports unimplemented");

        bool pipelineThrew = false;
        try {
            static_cast<void>(device.createGraphicsPipelineEx(dayo::graphics::PipelineDesc{}));
        } catch (const std::logic_error&) {
            pipelineThrew = true;
        }
        ok &= check(pipelineThrew, "mock typed pipeline reports unimplemented");

        bool sbtThrew = false;
        try {
            static_cast<void>(device.createShaderBindingTable(dayo::graphics::ShaderBindingTableDesc{}));
        } catch (const std::logic_error&) {
            sbtThrew = true;
        }
        ok &= check(sbtThrew, "mock SBT reports unimplemented");

        bool copyThrew = false;
        try {
            device.copyTextureEx(dayo::graphics::handles::TextureHandle{}, dayo::graphics::handles::TextureHandle{});
        } catch (const std::logic_error&) {
            copyThrew = true;
        }
        ok &= check(copyThrew, "mock typed copy reports unimplemented");

        bool readbackThrew = false;
        try {
            static_cast<void>(device.readbackTextureEx(dayo::graphics::handles::TextureHandle{}, 0, 0));
        } catch (const std::logic_error&) {
            readbackThrew = true;
        }
        ok &= check(readbackThrew, "mock typed readback reports unimplemented");

        bool uploadThrew = false;
        try {
            const std::array<std::uint8_t, 4> pixels{0, 0, 0, 255};
            device.uploadTextureEx(dayo::graphics::handles::TextureHandle{}, pixels, 0, 0);
        } catch (const std::logic_error&) {
            uploadThrew = true;
        }
        ok &= check(uploadThrew, "mock typed upload reports unimplemented");

        bool externalThrew = false;
        try {
            static_cast<void>(device.importExternalTexture(dayo::graphics::ExternalTextureImportDesc{}));
        } catch (const std::logic_error&) {
            externalThrew = true;
        }
        ok &= check(externalThrew, "mock external import reports unimplemented");

        // Legacy Preview contract still works on the mock.
        bool legacyOk = true;
        try {
            const BufferDesc bufferDesc{64, BufferDesc::Usage::vertex, true};
            const TextureDesc textureDesc{16, 16, TextureDesc::Format::rgba8Unorm, false, true};
            static_cast<void>(device.createBuffer(bufferDesc));
            static_cast<void>(device.createTexture(textureDesc));
            device.updatePreviewScene(PreviewScene{});
            device.clearPreviewResources();
        } catch (const std::exception& exception) {
            std::cerr << "FAIL: mock legacy preview path threw: " << exception.what() << '\n';
            legacyOk = false;
        }
        ok &= check(legacyOk, "mock legacy preview contract stays usable");
    }

    // ---- usage helpers ----
    {
        ok &= check(dayo::graphics::isWriteUsage(ResourceUsage::colorAttachment) &&
                        dayo::graphics::isWriteUsage(ResourceUsage::storageReadWrite) &&
                        dayo::graphics::isWriteUsage(ResourceUsage::present),
                    "write usage classification");
        ok &= check(!dayo::graphics::isWriteUsage(ResourceUsage::sampledRead) &&
                        !dayo::graphics::isWriteUsage(ResourceUsage::uniformRead) &&
                        !dayo::graphics::isWriteUsage(ResourceUsage::vertexRead),
                    "read usage is not write");
        ok &= check(dayo::graphics::isReadUsage(ResourceUsage::sampledRead) &&
                        dayo::graphics::isReadUsage(ResourceUsage::rayTracingRead) &&
                        dayo::graphics::isReadUsage(ResourceUsage::hostRead),
                    "read usage classification");
    }

    // ---- aliasing with lifetime overlap: disjoint transient may alias ----
    {
        RenderGraph graph;
        const auto early = graph.createResource("early", makeTransientDesc());
        const auto late = graph.createResource("late", makeTransientDesc());
        const auto overlapping = graph.createResource("overlapping", makeTransientDesc());
        const auto history = graph.createResource("history", makePersistentDesc());
        static_cast<void>(graph.addPass("p0", {{early, ResourceUsage::colorAttachment}}));
        static_cast<void>(
            graph.addPass("p1", {{early, ResourceUsage::sampledRead}, {overlapping, ResourceUsage::storageWrite}}));
        static_cast<void>(graph.addPass("p2", {{overlapping, ResourceUsage::storageRead}}));
        static_cast<void>(graph.addPass("p3", {{late, ResourceUsage::colorAttachment}}));
        static_cast<void>(graph.addPass("p4", {{late, ResourceUsage::sampledRead}}));
        static_cast<void>(graph.addPass("p5", {{overlapping, ResourceUsage::sampledRead}}));
        static_cast<void>(graph.addPass("p6", {{history, ResourceUsage::sampledRead}}));
        // early: 0-1, overlapping: 1-5, late: 3-4, history: 6-6.
        ok &= check(graph.canAlias(early, late), "transient pair passes lifetime-agnostic check");
        ok &= check(graph.canAliasWithLifetimes(early, late), "disjoint transient lifetimes may alias");
        ok &= check(!graph.canAliasWithLifetimes(early, overlapping), "overlapping transient lifetimes must not alias");
        ok &= check(!graph.canAliasWithLifetimes(overlapping, late),
                    "overlapping transient lifetimes must not alias (late)");
        ok &=
            check(!graph.canAliasWithLifetimes(early, history), "persistent history never aliases even when disjoint");
        const auto groups = graph.computeAliasGroups();
        ok &= check(groups.size() == 4, "alias groups cover all resources");
        if (groups.size() == 4) {
            ok &= check(groups[early] == groups[late], "disjoint transient shares alias group");
            ok &= check(groups[overlapping] != groups[early] && groups[overlapping] != groups[late],
                        "overlapping transient gets its own alias group");
            ok &= check(groups[history] != groups[early] && groups[history] != groups[late] &&
                            groups[history] != groups[overlapping],
                        "persistent history gets its own alias group");
        }
        // Buffer resources participate in the same lifetime model.
        RenderGraph buffers;
        BufferResourceDesc transientBuffer{256, ResourceUsage::storageReadWrite, false, ResourceLifetime::transient};
        BufferResourceDesc persistentBuffer{256, ResourceUsage::storageReadWrite, false, ResourceLifetime::persistent};
        const auto bufA = buffers.createBufferResource("buf-a", transientBuffer);
        const auto bufB = buffers.createBufferResource("buf-b", transientBuffer);
        const auto bufHistory = buffers.createBufferResource("buf-history", persistentBuffer);
        static_cast<void>(buffers.addPass("p0", {{bufA, ResourceUsage::storageWrite}}));
        static_cast<void>(buffers.addPass("p1", {{bufB, ResourceUsage::storageWrite}}));
        static_cast<void>(buffers.addPass("p2", {{bufHistory, ResourceUsage::storageRead}}));
        ok &= check(buffers.canAliasWithLifetimes(bufA, bufB), "disjoint transient buffers may alias");
        ok &= check(!buffers.canAliasWithLifetimes(bufA, bufHistory), "persistent buffers never alias");
        ok &= check(buffers.resourceBufferDesc(bufA) != nullptr && buffers.resourceBufferDesc(bufA)->size == 256,
                    "buffer descriptor round trip");
    }

    // ---- retirement timing: destroyed resources survive in-flight frames ----
    {
        dayo::graphics::TextureRetirementQueue textures;
        dayo::graphics::handles::TexturePool pool;
        const auto handle = pool.create();
        textures.retire(handle, 10);
        ok &= check(textures.pendingCount() == 1, "retired texture is pending");
        ok &= check(textures.sweep(11).empty(), "one frame later is still in flight");
        ok &= check(textures.pendingCount() == 1, "unready sweep keeps the entry");
        const auto ready = textures.sweep(12);
        ok &= check(ready.size() == 1 && ready.front() == handle, "retired texture frees after keepFrames");
        ok &= check(textures.pendingCount() == 0, "sweep drains completed entries");
        // Stale handles are still detectable at the pool after retirement.
        ok &= check(pool.isAlive(handle), "pool handle alive until explicit destroy");
        ok &= check(pool.destroy(handle) && !pool.isAlive(handle), "explicit destroy invalidates handle");

        dayo::graphics::BufferRetirementQueue buffers(1);
        dayo::graphics::handles::BufferPool bufferPool;
        const auto buffer = bufferPool.create();
        buffers.retire(buffer, 7);
        ok &= check(buffers.sweep(7).empty(), "same-frame sweep with keepFrames=1 stays pending");
        const auto bufferReady = buffers.sweep(8);
        ok &= check(bufferReady.size() == 1 && bufferReady.front() == buffer, "buffer frees exactly after keepFrames");
    }

    // ---- VMA allocation hooks ----
    {
        TextureResourceDesc volume = makeTransientDesc();
        volume.dimension = TextureDimension::d3;
        volume.extent = {32, 32, 16};
        volume.mipLevels = 4;
        volume.format = dayo::graphics::PixelFormat::rgba8Unorm;
        const std::size_t mip0 = 32U * 32U * 16U * 4U;
        const std::size_t mip1 = 16U * 16U * 8U * 4U;
        ok &=
            check(dayo::graphics::estimateTextureBytes(volume) >= mip0 + mip1, "VMA size estimate covers 3D mip chain");
        TextureResourceDesc array = makeTransientDesc();
        array.arrayLayers = 6;
        array.extent = {64, 64, 1};
        ok &= check(dayo::graphics::estimateTextureBytes(array) ==
                        6U * dayo::graphics::estimateTextureBytes(makeTransientDesc()),
                    "VMA size estimate scales with array layers");
        BufferResourceDesc bufferDesc{1024, ResourceUsage::uniformRead, false, ResourceLifetime::transient};
        ok &= check(dayo::graphics::estimateBufferBytes(bufferDesc) == 1024, "VMA buffer estimate is exact");
        ok &= check(dayo::graphics::isDepthFormat(dayo::graphics::PixelFormat::depth32Float) &&
                        !dayo::graphics::isDepthFormat(dayo::graphics::PixelFormat::rgba8Unorm),
                    "depth format classification");
    }

    // ---- Device拡張 (part 2): copy paths + layouts fail explicitly ----
    {
        MockDevice device;
        bool copyBufferThrew = false;
        try {
            device.copyBufferEx(dayo::graphics::handles::BufferHandle{}, dayo::graphics::handles::BufferHandle{});
        } catch (const std::logic_error&) {
            copyBufferThrew = true;
        }
        ok &= check(copyBufferThrew, "mock buffer copy reports unimplemented");
        bool bufferToTextureThrew = false;
        try {
            device.copyBufferToTextureEx(dayo::graphics::handles::BufferHandle{},
                                         dayo::graphics::handles::TextureHandle{});
        } catch (const std::logic_error&) {
            bufferToTextureThrew = true;
        }
        ok &= check(bufferToTextureThrew, "mock buffer-to-texture copy reports unimplemented");
        bool textureToBufferThrew = false;
        try {
            device.copyTextureToBufferEx(dayo::graphics::handles::TextureHandle{},
                                         dayo::graphics::handles::BufferHandle{});
        } catch (const std::logic_error&) {
            textureToBufferThrew = true;
        }
        ok &= check(textureToBufferThrew, "mock texture-to-buffer copy reports unimplemented");
        bool setLayoutThrew = false;
        try {
            static_cast<void>(device.createDescriptorSetLayoutEx(dayo::graphics::DescriptorSetLayoutDesc{}));
        } catch (const std::logic_error&) {
            setLayoutThrew = true;
        }
        ok &= check(setLayoutThrew, "mock descriptor set layout reports unimplemented");
        bool pipelineLayoutThrew = false;
        try {
            static_cast<void>(device.createPipelineLayoutEx(dayo::graphics::PipelineLayoutDesc{}));
        } catch (const std::logic_error&) {
            pipelineLayoutThrew = true;
        }
        ok &= check(pipelineLayoutThrew, "mock pipeline layout reports unimplemented");
        bool clearBufferThrew = false;
        try {
            device.clearBufferEx(dayo::graphics::handles::BufferHandle{}, 0);
        } catch (const std::logic_error&) {
            clearBufferThrew = true;
        }
        ok &= check(clearBufferThrew, "mock buffer clear reports unimplemented");
        bool retireThrew = false;
        try {
            device.retireTextureEx(dayo::graphics::handles::TextureHandle{}, 0);
        } catch (const std::logic_error&) {
            retireThrew = true;
        }
        ok &= check(retireThrew, "mock texture retirement reports unimplemented");
    }

    if (!ok)
        return 1;
    return 0;
}
