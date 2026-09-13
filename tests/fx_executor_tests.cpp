#include "fx/fx_catalog.hpp"
#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"
#include "fx/fx_preview_path.hpp"
#include "fx/fx_scheduler.hpp"
#include "fx/fx_shader_cache.hpp"
#include "fx/fx_shader_compiler.hpp"
#include "fx/fx_texture_cache.hpp"
#include "fx/fx_watcher.hpp"
#include "graphics/fx_executor.hpp"
#include "graphics/fx_pipeline_runtime.hpp"
#include "graphics/fx_resource_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
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
    dayo::graphics::handles::TextureHandle
    createTextureEx(const dayo::graphics::TextureResourceDesc& desc) override {
        textureDescs_.push_back(desc);
        return {nextTypedHandle_++, 1};
    }
    dayo::graphics::handles::BufferHandle
    createBufferEx(const dayo::graphics::BufferResourceDesc& desc) override {
        bufferDescs_.push_back(desc);
        return {nextTypedHandle_++, 1};
    }
    dayo::graphics::handles::SamplerHandle
    createSamplerEx(const dayo::graphics::SamplerResourceDesc& desc) override {
        samplerDescs_.push_back(desc);
        return {nextTypedHandle_++, 1};
    }
    void destroyTextureEx(dayo::graphics::handles::TextureHandle handle) override {
        if (handle.valid())
            ++destroyedTextures_;
    }
    void destroyBufferEx(dayo::graphics::handles::BufferHandle handle) override {
        if (handle.valid())
            ++destroyedBuffers_;
    }
    void destroySamplerEx(dayo::graphics::handles::SamplerHandle handle) override {
        if (handle.valid())
            ++destroyedSamplers_;
    }
    dayo::graphics::handles::DescriptorSetLayoutHandle
    createDescriptorSetLayoutEx(const dayo::graphics::DescriptorSetLayoutDesc& desc) override {
        descriptorLayout_ = desc;
        return {nextTypedHandle_++, 1};
    }
    void destroyDescriptorSetLayoutEx(dayo::graphics::handles::DescriptorSetLayoutHandle handle) override {
        if (handle.valid())
            ++destroyedDescriptorLayouts_;
    }
    dayo::graphics::handles::DescriptorSetHandle
    allocateDescriptorSetEx(dayo::graphics::handles::DescriptorSetLayoutHandle layout,
                            std::span<const dayo::graphics::DescriptorBindingEx> bindings) override {
        if (!layout.valid())
            throw std::invalid_argument("mock FX resource layout is invalid");
        descriptorBindings_.assign(bindings.begin(), bindings.end());
        return {nextTypedHandle_++, 1};
    }
    void destroyDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle handle) override {
        if (handle.valid())
            ++destroyedDescriptorSets_;
    }
    dayo::graphics::handles::ShaderHandle createShaderEx(const dayo::graphics::ShaderDesc&) override {
        return {nextTypedHandle_++, 1};
    }
    void destroyShaderEx(dayo::graphics::handles::ShaderHandle) override {
        ++destroyedShaders_;
    }
    dayo::graphics::handles::PipelineHandle
    createGraphicsPipelineEx(const dayo::graphics::GraphicsPipelineDescEx&) override {
        return {nextTypedHandle_++, 1};
    }
    dayo::graphics::handles::PipelineHandle
    createComputePipelineEx(const dayo::graphics::ComputePipelineDescEx&) override {
        return {nextTypedHandle_++, 1};
    }
    dayo::graphics::handles::PipelineHandle
    createRayTracingPipelineEx(const dayo::graphics::RayTracingPipelineDescEx&) override {
        return {nextTypedHandle_++, 1};
    }
    void destroyPipelineEx(dayo::graphics::handles::PipelineHandle) override {
        ++destroyedPipelines_;
    }
    dayo::graphics::handles::ShaderBindingTableHandle
    createShaderBindingTable(const dayo::graphics::ShaderBindingTableDesc&) override {
        return {nextTypedHandle_++, 1};
    }
    void destroyShaderBindingTable(dayo::graphics::handles::ShaderBindingTableHandle) override {
        ++destroyedSbt_;
    }
    dayo::graphics::DeviceCapabilities capabilities_;
    dayo::graphics::GraphicsConvention convention_;
    std::uint32_t nextTypedHandle_{1};
    std::size_t destroyedShaders_{};
    std::size_t destroyedPipelines_{};
    std::size_t destroyedSbt_{};
    std::size_t destroyedTextures_{};
    std::size_t destroyedBuffers_{};
    std::size_t destroyedSamplers_{};
    std::size_t destroyedDescriptorLayouts_{};
    std::size_t destroyedDescriptorSets_{};
    std::vector<dayo::graphics::TextureResourceDesc> textureDescs_;
    std::vector<dayo::graphics::BufferResourceDesc> bufferDescs_;
    std::vector<dayo::graphics::SamplerResourceDesc> samplerDescs_;
    dayo::graphics::DescriptorSetLayoutDesc descriptorLayout_;
    std::vector<dayo::graphics::DescriptorBindingEx> descriptorBindings_;
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
    void bindResources(std::span<const dayo::graphics::DescriptorBinding>) override {}
    void pushConstants(std::span<const std::byte>) override {}
    void copyTexture(dayo::graphics::TextureHandle, dayo::graphics::TextureHandle) override {
        trace.emplace_back("copy");
    }
    void clearTexture(dayo::graphics::TextureHandle) override {
        trace.emplace_back("clear");
    }
    void generateMipmaps(dayo::graphics::TextureHandle) override {
        trace.emplace_back("mipmap");
    }
    void copyTextureEx(dayo::graphics::handles::TextureHandle, dayo::graphics::handles::TextureHandle) override {
        trace.emplace_back("copyEx");
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle) override {
        trace.emplace_back("clearEx");
    }
    void generateMipmapsEx(dayo::graphics::handles::TextureHandle) override {
        trace.emplace_back("mipmapEx");
    }
    void bindDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle, std::uint32_t) override {
        trace.emplace_back("descriptorEx");
    }
    void bindPipelineEx(dayo::graphics::handles::PipelineHandle) override {
        trace.emplace_back("bindEx");
    }
    void transitionEx(dayo::graphics::handles::TextureHandle) override {
        trace.emplace_back("transitionEx");
    }
    void pushConstantsEx(std::span<const std::byte>) override {
        trace.emplace_back("pushEx");
    }
    void traceRaysEx(dayo::graphics::handles::PipelineHandle, dayo::graphics::handles::ShaderBindingTableHandle,
                     std::uint32_t, std::uint32_t, std::uint32_t) override {
        trace.emplace_back("traceEx");
    }
};

struct MinimalCommands final : public dayo::graphics::CommandList {
    void transition(dayo::graphics::TextureHandle) override {}
    void bindPipeline(dayo::graphics::PipelineHandle) override {}
    void draw(std::uint32_t, std::uint32_t) override {}
    void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void traceRays(std::uint32_t, std::uint32_t) override {}
};

dayo::fx::FxFrameContext testContext() {
    return dayo::fx::makeFxFrameContext(12.0F, 3, 64, 64, 1, 0, 90, 2, 2, 4);
}

dayo::graphics::FxExecutionResources testResources() {
    dayo::graphics::FxExecutionResources resources;
    resources.resolveTexture = [](std::string_view) -> std::optional<dayo::graphics::TextureHandle> { return 1; };
    resources.resolvePipeline = [](const dayo::fx::FxDispatch&) -> std::optional<dayo::graphics::PipelineHandle> {
        return 1;
    };
    return resources;
}

bool testMockTraceMatches() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    MockCommands commands;
    dayo::fx::FxProgram program;
    program.label = "trace";
    program.generation = 1;
    program.passes = {
        {"BG", dayo::fx::FxOpKind::clear, {}, 1, 1, {{"background", true}}, {}, {}, {}},
        {"MMD", dayo::fx::FxOpKind::raster, {}, 1, 1, {}, {}, {}, {}},
        {"DENOISE", dayo::fx::FxOpKind::compute, {}, 1, 1, {}, {}, {}, {}},
        {"Copy", dayo::fx::FxOpKind::copy, {}, 1, 1, {{"source", false}, {"destination", true}}, {}, {}, {}},
        {"Mip", dayo::fx::FxOpKind::mipmap, {}, 1, 1, {{"destination", true}}, {}, {}, {}},
    };
    dayo::fx::FxCompiler compiler;
    const auto plan = compiler.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext(), testResources());
    const std::vector<std::string> kinds = {"transition", "clear",      "bind", "draw",       "bind",
                                            "dispatch",   "transition", "copy", "transition", "mipmap"};
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
    rayProgram.passes.push_back({"RT", dayo::fx::FxOpKind::raytracing, {}, 1, 1, {}, {}, {}, {}});
    bool threw = false;
    try {
        const auto rayPlan = compiler.plan(rayProgram, testContext());
        static_cast<void>(executor.execute(rayPlan, commands, testContext(), testResources()));
    } catch (const dayo::graphics::FxRaytracingUnsupported&) {
        threw = true;
    }
    ok &= check(threw, "raytracing dispatch fails explicitly");

    MockCommands nativeCommands;
    dayo::graphics::FxExecutionResources nativeResources;
    nativeResources.resolveTypedPipeline = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::PipelineHandle>{{1, 1}};
    };
    nativeResources.resolveShaderBindingTable = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::ShaderBindingTableHandle>{{1, 1}};
    };
    dayo::fx::FxProgram nativeRayProgram;
    nativeRayProgram.passes.push_back({"NativeRT", dayo::fx::FxOpKind::raytracing, {}, 1, 1, {}, {}, {}, {}});
    const auto nativePlan = compiler.plan(nativeRayProgram, testContext());
    const auto nativeStats = executor.execute(nativePlan, nativeCommands, testContext(), nativeResources);
    ok &= check(nativeStats.rayTracing == 1 && nativeCommands.trace.size() == 2 &&
                    nativeCommands.trace[0] == "bindEx" && nativeCommands.trace[1] == "traceEx",
                "native RT executor forwards typed pipeline and SBT");

    MockCommands typedCommands;
    dayo::graphics::FxExecutionResources typedResources;
    typedResources.resolveTypedPipeline = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::PipelineHandle>{{2, 1}};
    };
    typedResources.resolveTypedTexture = [](std::string_view) {
        return std::optional<dayo::graphics::handles::TextureHandle>{{3, 1}};
    };
    typedResources.resolveDescriptorSet = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::DescriptorSetHandle>{{4, 1}};
    };
    dayo::fx::FxProgram typedProgram;
    typedProgram.passes = {
        {"typed-compute", dayo::fx::FxOpKind::compute, {}, 1, 1, {{"storage", true}}, {}, {}, {}},
        {"typed-copy", dayo::fx::FxOpKind::copy, {}, 1, 1, {{"source", false}, {"storage", true}}, {}, {}, {}},
        {"typed-clear", dayo::fx::FxOpKind::clear, {}, 1, 1, {{"storage", true}}, {}, {}, {}},
        {"typed-mipmap", dayo::fx::FxOpKind::mipmap, {}, 1, 1, {{"storage", true}}, {}, {}, {}},
    };
    const auto typedPlan = compiler.plan(typedProgram, testContext());
    const auto typedStats = executor.execute(typedPlan, typedCommands, testContext(), typedResources);
    ok &= check(typedStats.compute == 1 && typedStats.copy == 1 && typedStats.clear == 1 && typedStats.mipmap == 1,
                "typed executor counts compute and utility passes");
    ok &= check(std::count(typedCommands.trace.begin(), typedCommands.trace.end(), "bindEx") == 1 &&
                    std::count(typedCommands.trace.begin(), typedCommands.trace.end(), "descriptorEx") == 4 &&
                    std::count(typedCommands.trace.begin(), typedCommands.trace.end(), "transitionEx") == 4 &&
                    std::count(typedCommands.trace.begin(), typedCommands.trace.end(), "copyEx") == 1 &&
                    std::count(typedCommands.trace.begin(), typedCommands.trace.end(), "clearEx") == 1 &&
                    std::count(typedCommands.trace.begin(), typedCommands.trace.end(), "mipmapEx") == 1,
                "typed executor records pipeline, descriptor, transition, and utility commands");
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
    const auto stats = executor.execute(plan, commands, testContext(), testResources());
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
    initial.passes = {{"MMD", dayo::fx::FxOpKind::raster, {}, 1, 1, {}, {}, {}, {}}};
    dayo::fx::FxInstance instance(initial);
    bool ok = true;
    const auto empty = dayo::fx::makeFxSourceDocument("empty.fxdayo", "", 1);
    std::string error;
    ok &= check(!instance.tryHotReload(empty, testContext(), &error) && !error.empty(),
                "hot reload rejects empty source");
    ok &= check(instance.active()->generation == 7, "failed reload keeps current program");
    ok &= check(!instance.hasPending(), "failed reload stages nothing");
    const auto broken = dayo::fx::makeFxSourceDocument("broken.fxdayo", "[YRZFX]\n{ fx: { passes: [ }\n[HLSL]\n", 2);
    ok &= check(!instance.tryHotReload(broken, testContext(), &error) && !error.empty(),
                "hot reload rejects malformed raw source");
    ok &= check(instance.active()->generation == 7 && !instance.hasPending(),
                "malformed raw source keeps current program");
    // Successful reload stages on the worker path; only the explicit frame
    // boundary commit swaps the live program.
    const auto good = dayo::fx::makeFxSourceDocument("good.fxdayo",
                                                     R"FX([YRZFX]
{
  fx: {
    category: "render",
    passes: [{name: "from-raw", type: "compute", computeShader: "raw_cs"}],
  },
}
[HLSL]
raw_cs
)FX",
                                                     3);
    instance.setNextTimelineValue(42);
    ok &= check(instance.tryHotReload(good, testContext(), &error), "hot reload accepts valid source");
    ok &= check(instance.active()->generation == 7 && instance.hasPending(),
                "hot reload stages without swapping before frame boundary");
    ok &= check(instance.commitPendingAtFrameBoundary(), "frame boundary commits staged reload");
    ok &= check(instance.active()->generation == 8, "reload swaps generation at frame boundary");
    ok &= check(instance.active()->sourceVersion == 3, "committed program keeps source version");
    ok &= check(instance.retiredCount() == 1, "old program retires after swap");
    instance.retireCompleted(41);
    ok &= check(instance.retiredCount() == 1, "retire waits for timeline semaphore");
    instance.retireCompleted(42);
    ok &= check(instance.retiredCount() == 0, "retire drains after timeline passes");
    const auto stale = dayo::fx::makeFxSourceDocument("stale.fxdayo", good.raw, 2);
    ok &= check(!instance.stagePending(stale, &error) && error.find("stale") != std::string::npos,
                "out-of-order reload cannot replace a newer staged source");
    return ok;
}

bool testCompilerUsesRawSourceAndRejectsUnknownPasses() {
    const auto source = R"FX([YRZFX]
{
  fx: {
    category: "render",
    passes: [{name: "from-raw", type: "compute", computeShader: "raw_cs"}],
  },
}
[HLSL]
raw_cs
)FX";
    dayo::fx::FxCompiler compiler;
    const auto program = compiler.compileSource(dayo::fx::makeFxSourceDocument("missing/on-disk.fxdayo", source));
    bool ok = true;
    ok &= check(program.passes.size() == 1 && program.passes.front().name == "from-raw" &&
                    program.passes.front().kind == dayo::fx::FxOpKind::compute,
                "compiler parses caller-provided raw source");

    dayo::core::EffectGraph unknown;
    unknown.sourcePath = "unknown.fxdayo";
    dayo::core::EffectPass unsupported;
    unsupported.name = "unsupported";
    unsupported.type = dayo::core::EffectPassType::unknown;
    unknown.passes.push_back(unsupported);
    ok &= check(
        [&] {
            try {
                static_cast<void>(compiler.compile(unknown));
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        }(),
        "compiler rejects unknown pass types");

    const auto utilitySource = R"FX([YRZFX]
{
  fx: {
    category: "postprocess",
    passes: [{name: "copy", type: "copy", inputs: ["source"], RTV: ["destination"]}],
  },
}
[HLSL]
)FX";
    const auto utilityProgram = compiler.compileSource(dayo::fx::makeFxSourceDocument("copy.fxdayo", utilitySource));
    ok &=
        check(utilityProgram.passes.size() == 1 && utilityProgram.passes.front().kind == dayo::fx::FxOpKind::copy &&
                  utilityProgram.passes.front().resources.size() == 2 &&
                  !utilityProgram.passes.front().resources[0].write && utilityProgram.passes.front().resources[1].write,
              "source-driven copy preserves read/write resource contract");
    return ok;
}

bool testRayTracingPayloadIsLossless() {
    dayo::core::EffectGraph graph;
    graph.sourcePath = "lossless.fxdayo";
    dayo::core::EffectPass pass;
    pass.name = "native-rt";
    pass.type = dayo::core::EffectPassType::raytracing;
    pass.rayGenerationShader = "raygen_main";
    pass.missShaders = {"miss_primary", "miss_shadow"};
    pass.hitGroups = {
        {.type = "TRIANGLES", .closestHit = "closest_primary", .anyHit = "any_alpha", .intersection = ""},
        {.type = "PROCEDURAL", .closestHit = "closest_volume", .anyHit = "", .intersection = "intersect_volume"}};
    pass.callableShaders = {"sample_bsdf"};
    pass.maxPayloadSize = 128;
    pass.maxAttributeSize = 8;
    pass.maxRecursionDepth = 4;
    graph.passes.push_back(pass);

    dayo::fx::FxCompiler compiler;
    const auto program = compiler.compile(graph);
    bool ok = true;
    ok &= check(program.passes.size() == 1, "RT graph compiles one dispatch");
    if (program.passes.size() != 1)
        return false;
    const auto* ray = std::get_if<dayo::fx::FxRayTracingDispatch>(&program.passes.front().executable);
    ok &= check(ray != nullptr, "RT dispatch uses typed executable variant");
    if (ray == nullptr)
        return false;
    ok &= check(ray->rayGenerationShader == "raygen_main" && ray->missShaders.size() == 2,
                "RT raygen and miss shaders survive compilation");
    ok &= check(ray->hitGroups.size() == 2 &&
                    ray->hitGroups[0].type == dayo::core::fx::FxRayTracingHitGroupType::triangles &&
                    ray->hitGroups[0].anyHit == "any_alpha" &&
                    ray->hitGroups[1].type == dayo::core::fx::FxRayTracingHitGroupType::procedural &&
                    ray->hitGroups[1].intersection == "intersect_volume",
                "RT hit-group type and shader stages survive compilation");
    ok &= check(ray->callableShaders.size() == 1 && ray->callableShaders.front() == "sample_bsdf" &&
                    ray->maxPayloadSize == 128 && ray->maxAttributeSize == 8 && ray->maxRecursionDepth == 4,
                "RT callable shaders and limits survive compilation");
    return ok;
}

bool testFxResourceDeclarationsAreLossless() {
    dayo::core::EffectGraph graph;
    graph.sourcePath = "resources.fxdayo";
    graph.meshCloneCount = 4;
    dayo::core::EffectTexture color;
    color.name = "Color";
    color.format = "R16G16B16A16_FLOAT";
    color.view = "RTV";
    graph.textures.push_back(std::move(color));
    dayo::core::EffectTexture volume;
    volume.name = "Volume";
    volume.format = "R32_FLOAT";
    volume.view = "UAV";
    graph.textures3D.push_back(std::move(volume));
    dayo::core::EffectBuffer lights;
    lights.name = "Lights";
    lights.type = "float4";
    lights.view = "UAV";
    lights.elementSize = 16;
    graph.buffers.push_back(std::move(lights));
    dayo::core::EffectSampler linear;
    linear.name = "Linear";
    linear.filter = "LINEAR";
    linear.addressU = "CLAMP";
    linear.addressV = "CLAMP";
    graph.samplers.push_back(std::move(linear));
    dayo::core::EffectController exposure;
    exposure.name = "Exposure";
    exposure.controllerName = "controller.pmx";
    exposure.item = "Exposure";
    exposure.type = "float";
    graph.controllers.push_back(std::move(exposure));
    dayo::core::EffectPass pass;
    pass.name = "resource-pass";
    pass.type = dayo::core::EffectPassType::compute;
    pass.computeShader = "CS";
    pass.unorderedAccess.push_back({"Color", true});
    graph.passes.push_back(std::move(pass));

    const auto program = dayo::fx::FxCompiler{}.compile(graph);
    bool ok = true;
    ok &= check(program.textures.size() == 1 && program.textures.front().name == "Color" &&
                    program.textures.front().format == "R16G16B16A16_FLOAT",
                "compiled FX keeps 2D texture declarations");
    ok &= check(program.textures3D.size() == 1 && program.textures3D.front().name == "Volume",
                "compiled FX keeps 3D texture declarations");
    ok &= check(program.buffers.size() == 1 && program.buffers.front().elementSize == 16,
                "compiled FX keeps buffer declarations");
    ok &= check(program.samplers.size() == 1 && program.samplers.front().addressU == "CLAMP",
                "compiled FX keeps sampler declarations");
    ok &= check(program.controllers.size() == 1 && program.meshCloneCount == 4,
                "compiled FX keeps controller and cloning metadata");
    return ok;
}

bool testFxResourceRuntimeMaterializesDeclarations() {
    dayo::fx::FxProgram program;
    dayo::core::EffectTexture color;
    color.name = "Color";
    color.format = "R16G16B16A16_FLOAT";
    color.view = "RTV";
    color.size.absolute = true;
    color.size.width = 64;
    color.size.height = 32;
    program.textures.push_back(std::move(color));
    dayo::core::EffectTexture volume;
    volume.name = "Volume";
    volume.format = "R32_FLOAT";
    volume.view = "UAV";
    volume.size.absolute = true;
    volume.size.width = 8;
    volume.size.height = 4;
    volume.size.depth = 2;
    program.textures3D.push_back(std::move(volume));
    dayo::core::EffectBuffer lights;
    lights.name = "Lights";
    lights.type = "float4";
    lights.view = "UAV";
    lights.elementSize = 16;
    lights.size.absolute = true;
    lights.size.width = 4;
    program.buffers.push_back(std::move(lights));
    dayo::core::EffectSampler linear;
    linear.name = "Linear";
    linear.filter = "LINEAR";
    linear.addressU = "CLAMP";
    linear.addressV = "CLAMP";
    program.samplers.push_back(std::move(linear));

    MockDevice device;
    dayo::graphics::FxResourceRuntime runtime;
    std::string error;
    const auto context = testContext();
    bool ok = check(runtime.initialize(device, program, context, &error),
                    "FX resource runtime materializes declarations");
    ok &= check(error.empty(), "FX resource runtime has no initialization error");
    ok &= check(runtime.ready() && runtime.resourceCount() == 4, "FX resource runtime owns every declaration");
    const auto colorTexture = runtime.resolveTexture("Color");
    const auto volumeTexture = runtime.resolveTexture("Volume");
    const auto lightBuffer = runtime.resolveBuffer("Lights");
    const auto linearSampler = runtime.resolveSampler("Linear");
    ok &= check(colorTexture.has_value() && colorTexture->valid() && volumeTexture.has_value() &&
                    volumeTexture->valid(),
                "FX resource runtime resolves 2D and 3D textures");
    ok &= check(lightBuffer.has_value() && lightBuffer->valid() && linearSampler.has_value() &&
                    linearSampler->valid(),
                "FX resource runtime resolves buffers and samplers");
    const auto colorExtent = runtime.extent("Color");
    const auto volumeExtent = runtime.extent("Volume");
    ok &= check(colorExtent.has_value() && colorExtent->width == 64 && colorExtent->height == 32,
                "FX resource runtime resolves absolute 2D extents");
    ok &= check(volumeExtent.has_value() && volumeExtent->width == 8 && volumeExtent->height == 4 &&
                    volumeExtent->depth == 2,
                "FX resource runtime resolves absolute 3D extents");
    ok &= check(runtime.descriptorLayout().valid() && runtime.descriptorSet().valid() &&
                    runtime.descriptorLayoutDesc().bindings.size() == 4 && device.descriptorBindings_.size() == 4,
                "FX resource runtime allocates one typed descriptor set");
    ok &= check(device.descriptorLayout_.bindings[0].kind == dayo::graphics::DescriptorKind::sampledImage &&
                    device.descriptorLayout_.bindings[1].kind == dayo::graphics::DescriptorKind::storageImage &&
                    device.descriptorLayout_.bindings[2].kind == dayo::graphics::DescriptorKind::storageBuffer &&
                    device.descriptorLayout_.bindings[3].kind == dayo::graphics::DescriptorKind::sampler,
                "FX resource runtime derives descriptor kinds from views");
    dayo::fx::FxDispatch dispatch;
    ok &= check(runtime.resolveDescriptorSet(dispatch).has_value(), "FX resource runtime resolves pass descriptor set");
    runtime.reset();
    ok &= check(device.destroyedTextures_ == 2 && device.destroyedBuffers_ == 1 && device.destroyedSamplers_ == 1 &&
                    device.destroyedDescriptorLayouts_ == 1 && device.destroyedDescriptorSets_ == 1,
                "FX resource runtime releases all typed allocations");
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

bool testRealShaderCompilation() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;
    dayo::fx::FxShaderCompileRequest request;
    request.sourcePath = "compiler-test.hlsl";
    request.entryPoint = "main";
    request.stage = dayo::fx::FxShaderStage::fragment;
    request.hlsl = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n";
    const auto artifact = compiler.compile(request);
    bool ok = true;
    ok &=
        check(!artifact.spirv.empty() && artifact.spirv.front() == 0x07230203U, "real shader compiler returns SPIR-V");
    ok &= check(!artifact.compilerVersion.empty(), "real shader compiler records its version");

    dayo::fx::FxShaderCache cache;
    dayo::fx::FxShaderKey key;
    key.hlslHash = "compiler-test";
    key.entryPoint = request.entryPoint;
    key.stage = "fragment";
    const auto cached = cache.compileOrGet(key, request, compiler);
    const auto cachedAgain = cache.compileOrGet(key, request, compiler);
    ok &= check(cached.spirv == cachedAgain.spirv, "compiled shader cache reuses SPIR-V");
    const auto handle = cache.find(key);
    ok &= check(handle.has_value() && cache.binary(*handle).has_value(), "compiled shader binary is addressable");
    return ok;
}

bool testFxPipelineRuntime() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;
    MockDevice device;
    dayo::fx::FxProgram program;
    program.sourcePath = "pipeline-runtime.fxdayo";
    program.hlsl = "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {}\n";
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "deform";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.shader = "main";
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.macros = {"NATIVE=1"};
    program.passes.push_back(dispatch);

    dayo::graphics::FxPipelineRuntime runtime;
    std::string error;
    const bool built = runtime.build(
        device, program, compiler,
        [](const dayo::fx::FxDispatch&) {
            return std::optional<dayo::graphics::handles::PipelineLayoutHandle>{{1, 1}};
        },
        &error);
    bool ok = check(built, "FX pipeline runtime materializes a compute pipeline");
    ok &= check(error.empty() && runtime.size() == 1 && runtime.resolvePipeline(dispatch).has_value(),
                "FX pipeline runtime indexes the materialized pipeline");
    runtime.reset();
    ok &= check(device.destroyedPipelines_ == 1 && device.destroyedShaders_ == 1,
                "FX pipeline runtime destroys owned Vulkan objects");

    dayo::fx::FxDispatch postprocess;
    postprocess.name = "postprocess";
    postprocess.kind = dayo::fx::FxOpKind::postprocess;
    postprocess.executable = dayo::fx::FxPostProcessDispatch{"main"};
    program.passes = {postprocess};
    ok &= check(!runtime.build(
                    device, program, compiler,
                    [](const dayo::fx::FxDispatch&) {
                        return std::optional<dayo::graphics::handles::PipelineLayoutHandle>{{1, 1}};
                    },
                    &error) && error.find("fullscreen vertex") != std::string::npos,
                "FX pipeline runtime rejects postprocess without renderer fullscreen shader");
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
    // An omitted legacy implementation must fail explicitly instead of
    // silently dropping a native command.
    {
        MinimalCommands commands;
        bool rejected = false;
        try {
            commands.clearTexture(1);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        ok &= check(rejected, "legacy command-list defaults reject unsupported work");
    }
    ok &= testMockTraceMatches();
    ok &= testPreviewReferencePath();
    ok &= testSchedulerOrder();
    ok &= testCloneUnification();
    ok &= testWatcherReverseDeps();
    ok &= testHotReloadKeepsCurrentOnFailure();
    ok &= testCompilerUsesRawSourceAndRejectsUnknownPasses();
    ok &= testRayTracingPayloadIsLossless();
    ok &= testFxResourceDeclarationsAreLossless();
    ok &= testFxResourceRuntimeMaterializesDeclarations();
    ok &= testShaderCacheKeys();
    try {
        ok &= testRealShaderCompilation();
        ok &= testFxPipelineRuntime();
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: real shader compilation: " << exception.what() << '\n';
        ok = false;
    }
    ok &= testTextureCacheKeys();
    ok &= testWatcherPoll();
    if (ok)
        std::cout << "fx_executor: all checks passed\n";
    return ok ? 0 : 1;
}
