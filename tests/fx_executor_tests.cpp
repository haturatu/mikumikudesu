#include "fx/fx_catalog.hpp"
#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"
#include "fx/fx_preview_path.hpp"
#include "fx/fx_scheduler.hpp"
#include "fx/fx_shader_cache.hpp"
#include "fx/fx_texture_cache.hpp"
#include "fx/fx_watcher.hpp"
#include "graphics/fx_executor.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

struct MockDevice final : public dayo::graphics::Device {
    const dayo::graphics::DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    const dayo::graphics::GraphicsConvention& convention() const noexcept override {
        return convention_;
    }
    dayo::graphics::RendererKind activeRenderer() const noexcept override {
        return dayo::graphics::RendererKind::preview;
    }
    void selectRenderer(dayo::graphics::RendererKind) override {}
    void resize() override {}
    void beginUiFrame() override {}
    void renderFrame() override {}
    void waitIdle() override {}
    void uploadPreviewMesh(std::span<const dayo::graphics::PreviewVertex>, std::span<const std::uint32_t>) override {}
    void updatePreviewVertices(std::span<const dayo::graphics::PreviewVertex>) override {}
    void updatePreviewBones(std::span<const dayo::graphics::PreviewBoneTransform>) override {}
    void uploadPreviewMorphDeltas(std::span<const dayo::graphics::PreviewMorphDelta>) override {}
    void updatePreviewMorphWeights(std::span<const float>) override {}
    void updatePreviewMaterials(std::span<const dayo::graphics::PreviewMaterial>) override {}
    void updatePreviewDraws(std::span<const dayo::graphics::PreviewDraw>) override {}
    void uploadPreviewTextures(std::span<const dayo::graphics::PreviewTexture>) override {}
    void uploadPreviewBackground(std::span<const dayo::graphics::PreviewTexture>) override {}
    void clearPreviewResources() override {}
    void updatePreviewScene(const dayo::graphics::PreviewScene&) override {}
    dayo::graphics::BufferHandle createBuffer(const dayo::graphics::BufferDesc&) override {
        return 0;
    }
    dayo::graphics::TextureHandle createTexture(const dayo::graphics::TextureDesc&) override {
        return 0;
    }
    dayo::graphics::DeviceCapabilities capabilities_;
    dayo::graphics::GraphicsConvention convention_;
};

struct MockCommands final : public dayo::graphics::CommandList {
    std::vector<std::string> trace;
    void transition(dayo::graphics::TextureHandle) override {
        trace.emplace_back("transition");
    }
    void bindPipeline(dayo::graphics::PipelineHandle) override {
        trace.emplace_back("bind");
    }
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount) override {
        trace.push_back("draw:" + std::to_string(vertexCount) + "x" + std::to_string(instanceCount));
    }
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        trace.push_back("dispatch:" + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z));
    }
    void traceRays(std::uint32_t, std::uint32_t) override {
        trace.emplace_back("traceRays");
    }
    void copyTexture(dayo::graphics::TextureHandle, dayo::graphics::TextureHandle) override {
        trace.emplace_back("copy");
    }
    void clearTexture(dayo::graphics::TextureHandle) override {
        trace.emplace_back("clear");
    }
    void generateMipmaps(dayo::graphics::TextureHandle) override {
        trace.emplace_back("mipmap");
    }
};

dayo::fx::FxFrameContext testContext() {
    return dayo::fx::makeFxFrameContext(12.0F, 3, 64, 64, 1, 0, 90, 2, 2, 4);
}

bool testMockTraceMatches() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    MockCommands commands;
    dayo::fx::FxProgram program;
    program.label = "trace";
    program.generation = 1;
    program.passes = {
        {"BG", dayo::fx::FxOpKind::clear, {}},        {"MMD", dayo::fx::FxOpKind::raster, {}},
        {"DENOISE", dayo::fx::FxOpKind::compute, {}}, {"Copy", dayo::fx::FxOpKind::copy, {}},
        {"Mip", dayo::fx::FxOpKind::mipmap, {}},
    };
    dayo::fx::FxCompiler compiler;
    const auto plan = compiler.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext());
    const std::vector<std::string> kinds = {"clear", "draw", "dispatch", "copy", "mipmap"};
    bool ok = true;
    ok &= check(commands.trace.size() == kinds.size(), "mock trace length matches plan");
    for (std::size_t index = 0; index < kinds.size() && index < commands.trace.size(); ++index) {
        const bool prefix = commands.trace[index].starts_with(kinds[index]);
        ok &= check(prefix, "mock trace order matches plan");
    }
    ok &= check(stats.clear == 1 && stats.raster == 1 && stats.compute == 1 && stats.copy == 1 && stats.mipmap == 1,
                "executor stats count each kind");
    // Raytracing must fail explicitly, never silently skip.
    dayo::fx::FxProgram rayProgram = program;
    rayProgram.passes.push_back({"RT", dayo::fx::FxOpKind::raytracing, {}});
    bool threw = false;
    try {
        const auto rayPlan = compiler.plan(rayProgram, testContext());
        static_cast<void>(executor.execute(rayPlan, commands, testContext()));
    } catch (const dayo::graphics::FxRaytracingUnsupported&) {
        threw = true;
    }
    ok &= check(threw, "raytracing dispatch fails explicitly");
    return ok;
}

bool testPreviewReferencePath() {
    const auto plan = dayo::fx::buildPreviewReferencePlan(testContext());
    bool ok = true;
    ok &= check(plan.ordered.size() == 5, "preview reference has 5 passes");
    const std::vector<std::string> names = {"BG", "MMD", "GBuffer", "Copy", "DENOISE"};
    for (std::size_t index = 0; index < names.size() && index < plan.ordered.size(); ++index)
        ok &= check(plan.ordered[index].name == names[index], "preview reference order BG/MMD/GBuffer/Copy/DENOISE");
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    MockCommands commands;
    const auto stats = executor.execute(plan, commands, testContext());
    ok &= check(stats.clear == 1 && stats.raster == 2 && stats.copy == 1 && stats.compute == 1,
                "preview reference delegates plan->executor->backend");
    return ok;
}

bool testSchedulerOrder() {
    dayo::fx::EffectCatalog catalog;
    catalog.add({"Preview", dayo::fx::FxCategory::renderer, "renderer/Preview.fxdayo", true, 0});
    catalog.add({"Blur", dayo::fx::FxCategory::postprocess, "postprocess/Blur.fxdayo", true, 10});
    catalog.add({"tonemap", dayo::fx::FxCategory::postprocess, "postprocess/tonemap.fxdayo", true, 50});
    catalog.add({"Grain", dayo::fx::FxCategory::postprocess, "postprocess/Grain.fxdayo", true, 200});
    catalog.add({"Smoke", dayo::fx::FxCategory::particle, "particle/Smoke.fxdayo", true, 5});
    dayo::fx::FrameEffectScheduler scheduler;
    const auto scheduled = scheduler.schedule(catalog, "Preview");
    bool ok = true;
    ok &= check(!scheduled.empty() && scheduled.front().name == "deform" &&
                    scheduled.front().stage == dayo::fx::FrameStage::deform,
                "scheduler starts with deform of all models");
    ok &= check(scheduled.back().name == "present" && scheduled.back().stage == dayo::fx::FrameStage::present,
                "scheduler ends with present");
    const auto rank = [](dayo::fx::FrameStage stage) {
        switch (stage) {
        case dayo::fx::FrameStage::deform:
            return 0;
        case dayo::fx::FrameStage::renderer:
            return 1;
        case dayo::fx::FrameStage::postPre:
            return 2;
        case dayo::fx::FrameStage::tonemap:
            return 3;
        case dayo::fx::FrameStage::postPost:
            return 4;
        case dayo::fx::FrameStage::present:
            return 5;
        }
        return 9;
    };
    for (std::size_t index = 1; index < scheduled.size(); ++index)
        ok &= check(rank(scheduled[index - 1].stage) <= rank(scheduled[index].stage),
                    "scheduler order deform->renderer->postPre->tonemap->postPost->present");
    // Controller off skips the effect.
    scheduler.setControllerEnabled("Grain", false);
    const auto filtered = scheduler.schedule(catalog, "Preview");
    const bool grainGone =
        std::none_of(filtered.begin(), filtered.end(), [](const auto& entry) { return entry.name == "Grain"; });
    ok &= check(grainGone, "scheduler skips controller-off effects");
    ok &= check(scheduler.isEnabled("Blur") && !scheduler.isEnabled("Grain"), "controller enable query");
    return ok;
}

bool testCloneUnification() {
    bool ok = true;
    ok &= check(dayo::fx::unifyMeshCloneCount(2, 4) == 4, "clone unify takes effect maximum");
    ok &= check(dayo::fx::unifyMeshCloneCount(6, 4) == 6, "clone unify takes scene maximum");
    ok &= check(dayo::fx::unifyMeshCloneCount(0, 0) == 1, "clone unify clamps to 1");
    ok &= check(dayo::fx::unifyMeshCloneCount(2000, 1) == 1024, "clone unify clamps to scene range");
    ok &= check(dayo::fx::clonedVertexTotal(90, 4) == 360, "cloned vertex total scales");
    const auto context = dayo::fx::makeFxFrameContext(0.0F, 0, 64, 64, 7, 1, 90, 3, 1, 4);
    ok &=
        check(context.cloneCount == 4 && context.clonedVertexCount == 360, "frame context unifies scene/effect clones");
    return ok;
}

bool testWatcherReverseDeps() {
    dayo::fx::FxAssetWatcher watcher;
    watcher.addEffect("Preview", {"renderer/Preview.fxdayo", "common/resources.hlsli"});
    watcher.addEffect("Blur", {"postprocess/Blur.fxdayo", "common/resources.hlsli"});
    watcher.addEffect("Smoke", {"particle/Smoke.fxdayo"});
    bool ok = true;
    ok &= check(watcher.effectCount() == 3, "watcher tracks effects");
    const auto shared = watcher.notifyChanged("common/resources.hlsli");
    ok &= check(shared.size() == 2, "watcher recompiles only related effects via reverseDeps");
    ok &= check(watcher.hasDirty("Preview") && watcher.hasDirty("Blur") && !watcher.hasDirty("Smoke"),
                "watcher dirties dependents only");
    const auto dirty = watcher.takeDirty();
    ok &= check(dirty.size() == 2 && watcher.takeDirty().empty(), "watcher takeDirty drains");
    const auto single = watcher.notifyChanged("particle/Smoke.fxdayo");
    ok &= check(single.size() == 1 && single.front() == "Smoke", "watcher isolates unrelated effect");
    const auto none = watcher.notifyChanged("unrelated/file.hlsli");
    ok &= check(none.empty(), "watcher ignores unknown files");
    return ok;
}

bool testHotReloadKeepsCurrentOnFailure() {
    dayo::fx::FxProgram initial;
    initial.label = "Preview";
    initial.generation = 7;
    initial.passes = {{"MMD", dayo::fx::FxOpKind::raster, {}}};
    dayo::fx::FxInstance instance(initial);
    bool ok = true;
    const auto empty = dayo::fx::makeFxSourceDocument("empty.fxdayo", "", 1);
    std::string error;
    ok &= check(!instance.tryHotReload(empty, testContext(), &error) && !error.empty(),
                "hot reload rejects empty source");
    ok &= check(instance.active()->generation == 7, "failed reload keeps current program");
    ok &= check(!instance.hasPending(), "failed reload stages nothing");
    // Successful reload swaps only at frame boundary and retires old via timeline.
    const auto good = dayo::fx::makeFxSourceDocument("good.fxdayo", "pass MMD {}", 2);
    instance.setNextTimelineValue(42);
    ok &= check(instance.tryHotReload(good, testContext(), &error), "hot reload accepts valid source");
    ok &= check(instance.active()->generation == 8, "reload swaps generation at frame boundary");
    ok &= check(instance.retiredCount() == 1, "old program retires after swap");
    instance.retireCompleted(41);
    ok &= check(instance.retiredCount() == 1, "retire waits for timeline semaphore");
    instance.retireCompleted(42);
    ok &= check(instance.retiredCount() == 0, "retire drains after timeline passes");
    return ok;
}

bool testShaderCacheKeys() {
    dayo::fx::FxShaderCache cache;
    dayo::fx::FxShaderKey base;
    base.sourceHash = "dep-hash-1";
    base.hlslHash = "hlsl-hash-1";
    base.entryPoint = "main";
    base.stage = "pixel";
    base.dxcVersion = "dxc-1.8";
    constexpr const char* kSource = "float4 main() : SV_Target { return 1; }";
    bool ok = true;
    const auto first = cache.getOrCompile(base, kSource);
    ok &= check(cache.getOrCompile(base, kSource) == first, "shader cache dedups identical compilation");
    ok &= check(cache.findExact(base, kSource) == first, "shader cache exact lookup hits");
    ok &= check(!cache.findExact(base, "different source").has_value(), "shader cache exact lookup misses source");
    // Every key dimension must partition the cache.
    auto mutated = base;
    mutated.macros = "TONEMAP=1";
    ok &= check(cache.getOrCompile(mutated, kSource) != first, "macro set partitions shader cache");
    mutated = base;
    mutated.stage = "vertex";
    ok &= check(cache.getOrCompile(mutated, kSource) != first, "shader stage partitions cache");
    mutated = base;
    mutated.dxcVersion = "dxc-1.9";
    ok &= check(cache.getOrCompile(mutated, kSource) != first, "DXC version partitions cache");
    mutated = base;
    mutated.spirvTarget = "1.6";
    ok &= check(cache.getOrCompile(mutated, kSource) != first, "SPIR-V target partitions cache");
    mutated = base;
    mutated.compatProfile = "nativeExtended";
    ok &= check(cache.getOrCompile(mutated, kSource) != first, "compat profile partitions cache");
    mutated = base;
    mutated.hlslHash = "hlsl-hash-2";
    ok &= check(cache.getOrCompile(mutated, kSource) != first, "generated HLSL partitions cache");
    ok &= check(cache.size() == 7, "shader cache holds one entry per identity");

    dayo::fx::FxPipelineCache pipelines;
    dayo::fx::FxPipelineKey pipe;
    pipe.shader = base;
    pipe.renderTargetFormats = "rgba16f";
    pipe.depthFormat = "d32f";
    pipe.rasterState = "cull-back";
    pipe.deviceUuid = "amd-uuid";
    pipe.driver = "radv";
    const auto pipeFirst = pipelines.getOrCreate(pipe);
    ok &= check(pipelines.getOrCreate(pipe) == pipeFirst, "pipeline cache dedups identical key");
    auto pipeMutated = pipe;
    pipeMutated.renderTargetFormats = "rgba8";
    ok &= check(pipelines.getOrCreate(pipeMutated) != pipeFirst, "RT formats partition pipeline cache");
    pipeMutated = pipe;
    pipeMutated.deviceUuid = "other-uuid";
    ok &= check(pipelines.getOrCreate(pipeMutated) != pipeFirst, "device UUID partitions pipeline cache");
    return ok;
}

bool testTextureCacheKeys() {
    dayo::fx::FxTextureCache cache;
    dayo::fx::FxTextureKey base;
    base.path = "textures/face.png";
    base.contentHash = "mtime-123";
    bool ok = true;
    const auto first = cache.store(base, 64, 64);
    ok &= check(cache.store(base, 64, 64) == first, "texture cache dedups identical key");
    // Lexical canonicalization: same file via a different spelling hits.
    auto canonical = base;
    canonical.path = "textures/sub/../face.png";
    ok &= check(canonical.combined() == base.combined(), "texture key canonicalizes paths");
    ok &= check(cache.store(canonical, 64, 64) == first, "canonical path hits cache");
    auto mutated = base;
    mutated.contentHash = "mtime-456";
    ok &= check(cache.store(mutated, 64, 64) != first, "mtime/content hash partitions texture cache");
    mutated = base;
    mutated.colorSpace = "linear";
    ok &= check(cache.store(mutated, 64, 64) != first, "color space partitions texture cache");
    mutated = base;
    mutated.decodeFormat = "bc7";
    ok &= check(cache.store(mutated, 64, 64) != first, "decode format partitions texture cache");
    const auto found = cache.find(base);
    ok &= check(found.has_value() && found->width == 64 && found->height == 64, "texture metadata lookup hits");
    return ok;
}

bool testWatcherPoll() {
    namespace fs = std::filesystem;
    bool ok = true;
    const auto dir =
        fs::temp_directory_path() /
        ("dayo-fx-watch-" +
         std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code error;
    fs::create_directories(dir, error);
    if (error)
        return check(false, "watcher poll temp dir created");
    const auto shared = dir / "resources.hlsli";
    const auto solo = dir / "Smoke.fxdayo";
    {
        std::ofstream(shared) << "v1";
        std::ofstream(solo) << "v1";
    }
    dayo::fx::FxAssetWatcher watcher;
    watcher.addEffect("Preview", {"renderer/Preview.fxdayo", shared});
    watcher.addEffect("Blur", {"postprocess/Blur.fxdayo", shared});
    watcher.addEffect("Smoke", {solo});
    watcher.snapshot();
    ok &= check(watcher.poll().empty(), "watcher poll quiet without changes");
    // Bump the shared include mtime explicitly (mtime granularity varies).
    fs::last_write_time(shared, fs::file_time_type::clock::now() + std::chrono::seconds(5), error);
    ok &= check(!error, "watcher poll mtime bumped");
    const auto affected = watcher.poll();
    ok &= check(affected.size() == 2, "watcher poll recompiles only dependents of the include");
    ok &= check(watcher.hasDirty("Preview") && watcher.hasDirty("Blur") && !watcher.hasDirty("Smoke"),
                "watcher poll dirties dependents only");
    ok &= check(watcher.poll().empty(), "watcher poll drains after snapshot advance");
    fs::remove_all(dir, error);
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= testMockTraceMatches();
    ok &= testPreviewReferencePath();
    ok &= testSchedulerOrder();
    ok &= testCloneUnification();
    ok &= testWatcherReverseDeps();
    ok &= testHotReloadKeepsCurrentOnFailure();
    ok &= testShaderCacheKeys();
    ok &= testTextureCacheKeys();
    ok &= testWatcherPoll();
    if (ok)
        std::cout << "fx_executor: all checks passed\n";
    return ok ? 0 : 1;
}
