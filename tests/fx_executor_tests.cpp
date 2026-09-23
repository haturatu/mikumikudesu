#include "core/fx/fx_controller_resolver.hpp"
#include "core/fx/fx_material.hpp"
#include "fx/fx_catalog.hpp"
#include "fx/fx_compiler.hpp"
#include "fx/fx_condition_runtime.hpp"
#include "fx/fx_frame.hpp"
#include "fx/fx_preview_path.hpp"
#include "fx/fx_scheduler.hpp"
#include "fx/fx_shader_cache.hpp"
#include "fx/fx_shader_compiler.hpp"
#include "fx/fx_shader_source.hpp"
#include "fx/fx_texture_cache.hpp"
#include "fx/fx_watcher.hpp"
#include "graphics/dayo_fx_runtime.hpp"
#include "graphics/dayo_host_resources.hpp"
#include "graphics/fx_executor.hpp"
#include "graphics/fx_material_gpu_runtime.hpp"
#include "graphics/fx_material_scene_runtime.hpp"
#include "graphics/fx_pipeline_runtime.hpp"
#include "graphics/fx_resource_runtime.hpp"
#include "graphics/native_frame_constants.hpp"
#include "graphics/native_fx_global_variable_runtime.hpp"
#include "graphics/native_fx_runtime.hpp"
#include "graphics/native_oidn_provider.hpp"
#include "graphics/native_scene_bindings.hpp"
#include "graphics/native_scene_data.hpp"
#include "graphics/native_scene_derived_runtime.hpp"
#include "graphics/native_scene_frame_runtime.hpp"
#include "graphics/native_screen_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

bool hasUniqueDescriptorBindings(std::span<const std::uint32_t> words) {
    constexpr std::uint16_t opDecorate = 71;
    constexpr std::uint32_t bindingDecoration = 33;
    constexpr std::uint32_t descriptorSetDecoration = 34;
    std::map<std::uint32_t, std::uint32_t> bindings;
    std::map<std::uint32_t, std::uint32_t> sets;
    std::size_t offset = 5;
    while (offset < words.size()) {
        const auto instruction = words[offset];
        const auto wordCount = static_cast<std::size_t>(instruction >> 16U);
        if (wordCount == 0 || offset + wordCount > words.size())
            return false;
        if ((instruction & 0xFFFFU) == opDecorate && wordCount >= 4) {
            const auto target = words[offset + 1U];
            const auto decoration = words[offset + 2U];
            if (decoration == bindingDecoration)
                bindings[target] = words[offset + 3U];
            else if (decoration == descriptorSetDecoration)
                sets[target] = words[offset + 3U];
        }
        offset += wordCount;
    }
    std::set<std::pair<std::uint32_t, std::uint32_t>> locations;
    for (const auto& [target, binding] : bindings) {
        const auto set = sets.find(target);
        if (set != sets.end())
            locations.emplace(set->second, binding);
    }
    return locations.size() == bindings.size();
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
    dayo::graphics::handles::ShaderHandle nativeFullscreenVertexShader() const noexcept override {
        return {900, 1};
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
    dayo::graphics::handles::TextureHandle createTextureEx(const dayo::graphics::TextureResourceDesc& desc) override {
        textureDescs_.push_back(desc);
        return {nextTypedHandle_++, 1};
    }
    dayo::graphics::handles::BufferHandle createBufferEx(const dayo::graphics::BufferResourceDesc& desc) override {
        bufferDescs_.push_back(desc);
        return {nextTypedHandle_++, 1};
    }
    dayo::graphics::handles::SamplerHandle createSamplerEx(const dayo::graphics::SamplerResourceDesc& desc) override {
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
    void uploadTextureEx(dayo::graphics::handles::TextureHandle handle, std::span<const std::uint8_t> bytes,
                         std::uint32_t, std::uint32_t) override {
        lastUploadedTexture_ = handle;
        uploadedTextureBytes_ = bytes.size();
        uploadedTextureData_.assign(bytes.begin(), bytes.end());
        ++textureUploads_;
    }
    void uploadBufferEx(dayo::graphics::handles::BufferHandle handle, std::span<const std::byte> bytes,
                        std::size_t offset) override {
        bufferUploads_.push_back({handle, std::vector<std::byte>(bytes.begin(), bytes.end()), offset});
    }
    std::vector<std::byte> readbackBufferEx(dayo::graphics::handles::BufferHandle handle, std::size_t offset,
                                            std::size_t size) override {
        lastBufferReadbackHandle_ = handle;
        lastBufferReadbackOffset_ = offset;
        lastBufferReadbackSize_ = size;
        return bufferReadbackBytes_;
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle, const std::array<float, 4>&) override {}
    void clearBufferEx(dayo::graphics::handles::BufferHandle, std::uint32_t) override {
        ++bufferClears_;
    }
    void generateMipmapsEx(dayo::graphics::handles::TextureHandle) override {
        ++generatedMipmaps_;
    }
    dayo::graphics::handles::DescriptorSetLayoutHandle
    createDescriptorSetLayoutEx(const dayo::graphics::DescriptorSetLayoutDesc& desc) override {
        descriptorLayout_ = desc;
        descriptorLayoutsCreated_.push_back(desc);
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
        const dayo::graphics::handles::DescriptorSetHandle handle{nextTypedHandle_++, 1};
        descriptorAllocations_.push_back({handle, layout, {bindings.begin(), bindings.end()}});
        return handle;
    }
    void updateDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle set,
                               std::span<const dayo::graphics::DescriptorBindingEx> bindings) override {
        updatedDescriptorSets_.push_back(set);
        updatedDescriptorBindings_.emplace_back(bindings.begin(), bindings.end());
    }
    std::size_t currentFrameSlot() const noexcept override {
        return frameSlot_;
    }
    void destroyDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle handle) override {
        if (handle.valid())
            ++destroyedDescriptorSets_;
    }
    dayo::graphics::handles::PipelineLayoutHandle
    createPipelineLayoutEx(const dayo::graphics::PipelineLayoutDesc& desc) override {
        pipelineLayoutDesc_ = desc;
        return {nextTypedHandle_++, 1};
    }
    void destroyPipelineLayoutEx(dayo::graphics::handles::PipelineLayoutHandle handle) override {
        if (handle.valid())
            ++destroyedPipelineLayouts_;
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
    struct BufferUpload {
        dayo::graphics::handles::BufferHandle handle{};
        std::vector<std::byte> bytes;
        std::size_t offset{};
    };
    struct DescriptorSetAllocation {
        dayo::graphics::handles::DescriptorSetHandle set{};
        dayo::graphics::handles::DescriptorSetLayoutHandle layout{};
        std::vector<dayo::graphics::DescriptorBindingEx> bindings;
    };
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
    std::size_t destroyedPipelineLayouts_{};
    std::size_t uploadedTextureBytes_{};
    std::size_t textureUploads_{};
    dayo::graphics::handles::TextureHandle lastUploadedTexture_{};
    std::vector<std::uint8_t> uploadedTextureData_;
    dayo::graphics::handles::BufferHandle lastBufferReadbackHandle_{};
    std::size_t lastBufferReadbackOffset_{};
    std::size_t lastBufferReadbackSize_{};
    std::vector<std::byte> bufferReadbackBytes_;
    std::size_t bufferClears_{};
    std::size_t generatedMipmaps_{};
    std::vector<BufferUpload> bufferUploads_;
    std::vector<dayo::graphics::TextureResourceDesc> textureDescs_;
    std::vector<dayo::graphics::BufferResourceDesc> bufferDescs_;
    std::vector<dayo::graphics::SamplerResourceDesc> samplerDescs_;
    dayo::graphics::DescriptorSetLayoutDesc descriptorLayout_;
    std::vector<dayo::graphics::DescriptorBindingEx> descriptorBindings_;
    std::vector<dayo::graphics::DescriptorSetLayoutDesc> descriptorLayoutsCreated_;
    std::vector<DescriptorSetAllocation> descriptorAllocations_;
    std::vector<dayo::graphics::handles::DescriptorSetHandle> updatedDescriptorSets_;
    std::vector<std::vector<dayo::graphics::DescriptorBindingEx>> updatedDescriptorBindings_;
    std::size_t frameSlot_{};
    dayo::graphics::PipelineLayoutDesc pipelineLayoutDesc_;
};

struct MockCommands final : public dayo::graphics::CommandList {
    std::vector<std::string> trace;
    std::vector<dayo::graphics::IndexedDrawEx> indexedDraws;
    std::vector<dayo::graphics::VertexDrawEx> vertexBufferDraws;
    std::vector<std::uint32_t> descriptorSetIndices_;
    void transition(dayo::graphics::TextureHandle) override {
        trace.emplace_back("transition");
    }
    void bindPipeline(dayo::graphics::PipelineHandle) override {
        trace.emplace_back("bind");
    }
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount) override {
        trace.push_back("draw:" + std::to_string(vertexCount) + "x" + std::to_string(instanceCount));
    }
    void drawIndexedEx(const dayo::graphics::IndexedDrawEx& draw) override {
        indexedDraws.push_back(draw);
        trace.push_back("drawIndexedEx:" + std::to_string(draw.modelIndex) + ":" + std::to_string(draw.materialIndex));
    }
    void drawVertexBufferEx(const dayo::graphics::VertexDrawEx& draw) override {
        vertexBufferDraws.push_back(draw);
        trace.push_back("drawVertexBufferEx:" + std::to_string(draw.vertexCount) + "x" +
                        std::to_string(draw.instanceCount) + ":" + std::to_string(draw.vertexBuffers.size()));
    }
    void drawIndexedBufferlessEx(dayo::graphics::handles::BufferHandle, std::uint32_t indexCount,
                                 std::uint32_t instanceCount) override {
        trace.push_back("drawIndexedBufferlessEx:" + std::to_string(indexCount) + "x" + std::to_string(instanceCount));
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
    void blitTextureEx(dayo::graphics::handles::TextureHandle, dayo::graphics::handles::TextureHandle,
                       std::array<std::uint32_t, 4> sourceRect) override {
        trace.push_back("blitEx:" + std::to_string(sourceRect[0]) + ":" + std::to_string(sourceRect[1]) + ":" +
                        std::to_string(sourceRect[2]) + ":" + std::to_string(sourceRect[3]));
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle) override {
        trace.emplace_back("clearEx");
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle, const std::array<float, 4>&) override {
        trace.emplace_back("clearEx");
    }
    void clearBufferEx(dayo::graphics::handles::BufferHandle, const std::array<float, 4>& value) override {
        trace.push_back("clearBufferEx:" + std::to_string(value[0]) + ":" + std::to_string(value[1]) + ":" +
                        std::to_string(value[2]) + ":" + std::to_string(value[3]));
    }
    void generateMipmapsEx(dayo::graphics::handles::TextureHandle) override {
        trace.emplace_back("mipmapEx");
    }
    void transferBarrierEx() override {
        trace.emplace_back("transferBarrierEx");
    }
    void flushAndWaitForHostReadbackEx() override {
        trace.emplace_back("flushAndWaitForHostReadbackEx");
    }
    void memoryBarrierEx() override {
        trace.emplace_back("memoryBarrierEx");
    }
    void bindDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle, std::uint32_t setIndex) override {
        trace.emplace_back("descriptorEx");
        descriptorSetIndices_.push_back(setIndex);
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
    void beginRenderingEx(dayo::graphics::handles::TextureHandle, bool) override {
        trace.emplace_back("beginRenderingEx");
    }
    void beginRenderingEx(const dayo::graphics::RenderingInfoEx& info) override {
        trace.push_back("beginRenderingInfoEx:" + std::to_string(info.colors.size()));
    }
    void endRenderingEx() override {
        trace.emplace_back("endRenderingEx");
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

    MockCommands graphicsCommands;
    dayo::fx::FxProgram graphicsProgram;
    dayo::fx::FxDispatch postprocess;
    postprocess.name = "typed-postprocess";
    postprocess.kind = dayo::fx::FxOpKind::postprocess;
    postprocess.executable = dayo::fx::FxPostProcessDispatch{"main", {}};
    postprocess.resources = {{"source", false}, {"target", true}};
    graphicsProgram.passes.push_back(postprocess);
    const auto graphicsPlan = dayo::fx::FxCompiler{}.plan(graphicsProgram, testContext());
    const auto graphicsStats = executor.execute(graphicsPlan, graphicsCommands, testContext(), typedResources);
    ok &= check(graphicsStats.postprocess == 1 &&
                    graphicsCommands.trace == std::vector<std::string>{"transitionEx", "descriptorEx",
                                                                       "beginRenderingEx", "bindEx", "draw:3x1",
                                                                       "endRenderingEx", "memoryBarrierEx"},
                "typed graphics executor brackets postprocess draws with a render target");
    std::size_t beforePasses = 0;
    std::size_t afterPasses = 0;
    auto hookResources = testResources();
    hookResources.beforePass = [&beforePasses](const dayo::fx::FxDispatch&, dayo::graphics::CommandList&) {
        ++beforePasses;
    };
    hookResources.afterPass = [&afterPasses](const dayo::fx::FxDispatch&, dayo::graphics::CommandList&) {
        ++afterPasses;
    };
    dayo::fx::FxProgram hookProgram;
    hookProgram.passes.push_back({"hooked", dayo::fx::FxOpKind::compute, {}, 1, 1, {{"Input", false}}, {}, {}, {}});
    const auto hookPlan = dayo::fx::FxCompiler{}.plan(hookProgram, testContext());
    static_cast<void>(executor.execute(hookPlan, graphicsCommands, testContext(), hookResources));
    ok &= check(beforePasses == 1 && afterPasses == 1, "FX pass hooks run around executed generic passes");
    return ok;
}

bool testResolvedPassPlanning() {
    using namespace dayo;
    const fx::FxCompiler compiler;
    bool ok = true;

    fx::FxProgram screenProgram;
    fx::FxDispatch screenPass;
    screenPass.name = "screen-compute";
    screenPass.kind = fx::FxOpKind::compute;
    screenPass.executable = fx::FxComputeDispatch{"CS"};
    screenProgram.passes.push_back(screenPass);
    auto context = testContext();
    context.renderWidth = 65;
    context.renderHeight = 33;
    const auto screenPlan = compiler.plan(screenProgram, context);
    ok &= check(
        screenPlan.resolved.size() == 1 && screenPlan.resolved[0].outputExtent.width == 65 &&
            screenPlan.resolved[0].outputExtent.height == 33 && screenPlan.resolved[0].outputExtent.dimension == 2 &&
            screenPlan.resolved[0].numThreads == std::array<std::uint32_t, 3>{16, 16, 1} &&
            screenPlan.resolved[0].dispatchGroups.width == 5 && screenPlan.resolved[0].dispatchGroups.height == 3,
        "render compute resolves screen extent and upstream 2D numthreads defaults");

    fx::FxProgram deformProgram;
    deformProgram.category = core::fx::FxCategory::deform;
    fx::FxDispatch deformPass;
    deformPass.name = "deform-compute";
    deformPass.kind = fx::FxOpKind::compute;
    deformPass.category = core::fx::FxCategory::deform;
    deformPass.executable = fx::FxComputeDispatch{"CS"};
    deformProgram.passes.push_back(deformPass);
    auto deformContext = testContext();
    deformContext.vertexCount = 600;
    deformContext.clonedVertexCount = 2400;
    const auto deformPlan = compiler.plan(deformProgram, deformContext);
    ok &= check(deformPlan.resolved[0].outputExtent.width == deformContext.clonedVertexCount &&
                    deformPlan.resolved[0].outputExtent.dimension == 1 &&
                    deformPlan.resolved[0].numThreads == std::array<std::uint32_t, 3>{1024, 1, 1} &&
                    deformPlan.resolved[0].dispatchGroups.width == 3,
                "deform compute resolves cloned vertex count and upstream 1D numthreads defaults");

    fx::FxProgram relativeProgram;
    core::EffectTexture source;
    source.name = "Source";
    source.size.absolute = true;
    source.size.dimension = 2;
    source.size.width = 127;
    source.size.height = 65;
    relativeProgram.textures.push_back(source);
    fx::FxDispatch relativePass;
    relativePass.name = "relative-size";
    relativePass.kind = fx::FxOpKind::compute;
    relativePass.outputSize.base = "Source";
    relativePass.outputSize.widthRatio = 0.5F;
    relativePass.outputSize.heightRatio = 0.5F;
    relativePass.executable = fx::FxComputeDispatch{"CS"};
    relativeProgram.passes.push_back(relativePass);
    const auto relativePlan = compiler.plan(relativeProgram, context);
    ok &= check(relativePlan.resolved[0].outputExtent.width == 63 && relativePlan.resolved[0].outputExtent.height == 32,
                "relative pass output size applies upstream truncation and ratios");

    fx::FxProgram explicit3dProgram;
    fx::FxDispatch explicit3dPass;
    explicit3dPass.name = "explicit-3d";
    explicit3dPass.kind = fx::FxOpKind::compute;
    explicit3dPass.numThreads = {32, 0, 0};
    explicit3dPass.outputSize.absolute = true;
    explicit3dPass.outputSize.dimension = 3;
    explicit3dPass.outputSize.width = 17;
    explicit3dPass.outputSize.height = 9;
    explicit3dPass.outputSize.depth = 5;
    explicit3dPass.executable = fx::FxComputeDispatch{"CS"};
    explicit3dProgram.passes.push_back(explicit3dPass);
    const auto explicit3dPlan = compiler.plan(explicit3dProgram, context);
    ok &= check(explicit3dPlan.resolved[0].numThreads == std::array<std::uint32_t, 3>{32, 1, 1} &&
                    explicit3dPlan.resolved[0].dispatchGroups.width == 1 &&
                    explicit3dPlan.resolved[0].dispatchGroups.height == 9 &&
                    explicit3dPlan.resolved[0].dispatchGroups.depth == 5,
                "explicit 3D output and partially specified numthreads resolve all axes");

    fx::FxProgram inferredVolumeProgram;
    core::EffectTexture volume;
    volume.name = "Volume";
    volume.format = "R32_FLOAT";
    volume.view = "UAV";
    volume.size.absolute = true;
    volume.size.dimension = 3;
    volume.size.width = 17;
    volume.size.height = 9;
    volume.size.depth = 5;
    inferredVolumeProgram.textures3D.push_back(volume);
    fx::FxDispatch inferredVolumePass;
    inferredVolumePass.name = "volume-compute";
    inferredVolumePass.kind = fx::FxOpKind::compute;
    inferredVolumePass.executable = fx::FxComputeDispatch{"CS"};
    inferredVolumePass.resources.push_back({"Volume", true, fx::FxResourceRole::storage});
    inferredVolumeProgram.passes.push_back(inferredVolumePass);
    const auto inferredVolumePlan = compiler.plan(inferredVolumeProgram, context);
    const auto generatedVolumeShader = fx::makeNativeFxShaderSource(inferredVolumeProgram, inferredVolumePass, 0, {},
                                                                    &inferredVolumePlan.resolved.front());
    ok &= check(inferredVolumePlan.resolved.front().numThreads == std::array<std::uint32_t, 3>{8, 8, 8} &&
                    inferredVolumePlan.resolved.front().dispatchGroups.width == 3 &&
                    inferredVolumePlan.resolved.front().dispatchGroups.height == 2 &&
                    inferredVolumePlan.resolved.front().dispatchGroups.depth == 1 &&
                    generatedVolumeShader.find("#define YRZ_NUMTHREADS [numthreads(8,8,8)]") != std::string::npos,
                "3D shader numthreads and dispatch groups both use the resolved volume extent");

    fx::FxProgram depthProgram;
    core::EffectTexture depthTexture;
    depthTexture.name = "Depth";
    depthTexture.format = "D24_UNORM_S8_UINT";
    depthTexture.view = "DSV";
    depthProgram.textures.push_back(depthTexture);
    fx::FxDispatch depthAttachmentPass;
    depthAttachmentPass.name = "depth-attachment";
    depthAttachmentPass.resources.push_back({"Depth", true, fx::FxResourceRole::depthAttachment});
    const auto depthAttachmentSource = fx::makeNativeFxShaderSource(depthProgram, depthAttachmentPass, 0);
    fx::FxDispatch depthSamplePass;
    depthSamplePass.name = "depth-sample";
    depthSamplePass.resources.push_back({"Depth", false, fx::FxResourceRole::sampled});
    const auto depthSampleSource = fx::makeNativeFxShaderSource(depthProgram, depthSamplePass, 0);
    ok &= check(depthAttachmentSource.find("Texture2D<float> Depth;") != std::string::npos &&
                    depthAttachmentSource.find("Depth : register(") == std::string::npos &&
                    depthSampleSource.find("Texture2D<float> Depth : register(t0") != std::string::npos,
                "D24S8 attachment stays unbound and is emitted as a sampled float texture in a later pass");

    fx::FxProgram materialShaderProgram;
    materialShaderProgram.materialDescriptor =
        core::EffectMaterialDescriptor{.name = "Surface", .templatePath = {}, .defaultFile = {}};
    materialShaderProgram.materialSchema = core::fx::parseMaterialTemplateSchema(
        "f.1 : Roughness\n_T0 : Albedo\n_V2 : Volume\nRoughness : 0.5\n", "Surface");
    core::EffectTexture materialLookupTexture;
    materialLookupTexture.name = "Lookup";
    materialShaderProgram.textures.push_back(materialLookupTexture);
    fx::FxDispatch materialShaderPass;
    materialShaderPass.name = "material-shader";
    const auto materialBindingPlan = fx::planPassBindings(materialShaderProgram, materialShaderPass, 0);
    const auto materialShader = fx::makeNativeFxShaderSource(materialShaderProgram, materialShaderPass, 0);
    ok &=
        check(materialBindingPlan.material.has_value() && materialBindingPlan.material->materialIndices.binding == 17 &&
                  materialBindingPlan.material->textureIndices2D.binding == 18 &&
                  materialBindingPlan.material->textureIndices3D.binding == 19 &&
                  materialBindingPlan.material->values.binding == 20 &&
                  materialBindingPlan.material->textures2D.binding == 21 &&
                  materialBindingPlan.material->textures3D.set == 1 &&
                  materialBindingPlan.material->textures3D.binding == 16 &&
                  materialShader.find("Lookup : register(t0, space0)") != std::string::npos &&
                  materialShader.find("Surface_idx : register(t1, space0)") != std::string::npos &&
                  materialShader.find("Surface_texture[] : register(t5, space0)") != std::string::npos &&
                  materialShader.find("Surface_texture3D[] : register(t0, space1)") != std::string::npos &&
                  materialShader.find("result.Albedo = Surface_texture[NonUniformResourceIndex(result.hasAlbedo ? "
                                      "SurfaceTextureIndex_Albedo : 0)];") != std::string::npos &&
                  materialShader.find("result.Volume = Surface_texture3D[NonUniformResourceIndex(result.hasVolume ? "
                                      "SurfaceTextureIndex_Volume : 0)];") != std::string::npos,
              "MatDesc HLSL registers share the pass binding plan and use descriptor zero for missing textures");
    const auto* plannedMaterialIndices = materialBindingPlan.find("@matdesc/idx");
    const auto* plannedMaterialTextures3D = materialBindingPlan.find("@matdesc/texture3D");
    ok &= check(plannedMaterialIndices != nullptr && plannedMaterialIndices->set == 0 &&
                    plannedMaterialIndices->binding == 17 && plannedMaterialTextures3D != nullptr &&
                    plannedMaterialTextures3D->set == 1 && plannedMaterialTextures3D->binding == 16,
                "MatDesc logical descriptor resources are retained in the pass binding plan");

    fx::FxProgram textureOnlyMaterialProgram;
    textureOnlyMaterialProgram.materialDescriptor =
        core::EffectMaterialDescriptor{.name = "TextureOnly", .templatePath = {}, .defaultFile = {}};
    textureOnlyMaterialProgram.materialSchema = core::fx::parseMaterialTemplateSchema("_T0 : Albedo\n", "TextureOnly");
    fx::FxDispatch textureOnlyMaterialPass;
    textureOnlyMaterialPass.name = "texture-only-material";
    const auto textureOnlyMaterialShader =
        fx::makeNativeFxShaderSource(textureOnlyMaterialProgram, textureOnlyMaterialPass, 0);
    ok &= check(textureOnlyMaterialShader.find("struct TextureOnlyValue {\n    uint _DayoEmpty;\n};") !=
                    std::string::npos,
                "texture-only MatDesc emits a private value record compatible with its GPU table stride");

    fx::FxProgram bindingProgram;
    core::EffectTexture textureSrv;
    textureSrv.name = "TexSRV";
    core::EffectTexture textureUav;
    textureUav.name = "TexUAV";
    bindingProgram.textures = {textureSrv, textureUav};
    core::EffectBuffer bufferSrv;
    bufferSrv.name = "BufSRV";
    core::EffectBuffer bufferUav;
    bufferUav.name = "BufUAV";
    bindingProgram.buffers = {bufferSrv, bufferUav};
    core::EffectSampler sampler;
    sampler.name = "Linear";
    bindingProgram.samplers = {sampler};
    fx::FxDispatch bindingPass;
    bindingPass.resources = {{"TexSRV", false}, {"TexUAV", true}, {"BufSRV", false}, {"BufUAV", true}};
    const auto bindings = fx::planPassBindings(bindingProgram, bindingPass, 3);
    const auto bindingFor = [&bindings](std::string_view name) -> const fx::FxLogicalBinding* {
        return bindings.find(name);
    };
    ok &= check(bindingFor("TexSRV") != nullptr && bindingFor("TexSRV")->set == 3 &&
                    bindingFor("TexSRV")->binding == 16 && bindingFor("BufSRV") != nullptr &&
                    bindingFor("BufSRV")->binding == 17 && bindingFor("TexUAV") != nullptr &&
                    bindingFor("TexUAV")->binding == 0 && bindingFor("BufUAV") != nullptr &&
                    bindingFor("BufUAV")->binding == 1 && bindingFor("Linear") != nullptr &&
                    bindingFor("Linear")->binding == 32,
                "pass binding plan shares HLSL t/u register namespaces across images and buffers");
    return ok;
}

bool testRasterModelTargetIndexedDraws() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    MockCommands commands;

    dayo::fx::FxDispatch dispatch;
    dispatch.name = "indexed-raster";
    dispatch.kind = dayo::fx::FxOpKind::raster;
    dayo::fx::FxRasterDispatch raster;
    raster.vertexShader = "VS";
    raster.pixelShader = "PS";
    raster.graphics.modelTarget = dayo::core::fx::RasterModelTarget::self;
    dispatch.executable = raster;
    dispatch.resources = {{"Color", true, dayo::fx::FxResourceRole::colorAttachment}};
    dayo::fx::FxProgram program;
    program.passes.push_back(dispatch);

    const std::array<dayo::graphics::NativeSceneDraw, 3> draws = {
        dayo::graphics::NativeSceneDraw{.vertexBuffer = {10, 1},
                                        .indexBuffer = {11, 1},
                                        .modelIndex = 7,
                                        .materialIndex = 2,
                                        .firstIndex = 12,
                                        .indexCount = 36},
        dayo::graphics::NativeSceneDraw{.vertexBuffer = {10, 1},
                                        .indexBuffer = {11, 1},
                                        .modelIndex = 8,
                                        .materialIndex = 3,
                                        .firstIndex = 48,
                                        .indexCount = 12},
        dayo::graphics::NativeSceneDraw{.vertexBuffer = {10, 1},
                                        .indexBuffer = {11, 1},
                                        .modelIndex = 7,
                                        .materialIndex = 4,
                                        .firstIndex = 60,
                                        .indexCount = 6},
    };
    dayo::graphics::FxExecutionResources resources;
    resources.sceneDraws = draws;
    resources.rasterControllerModel = 7;
    std::vector<std::uint32_t> passModels;
    resources.updatePassConstants = [&passModels](dayo::graphics::CommandList&,
                                                  const dayo::graphics::NativeSceneDraw& draw) {
        passModels.push_back(draw.modelIndex);
    };
    resources.resolveTypedPipeline = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::PipelineHandle>{{20, 1}};
    };
    resources.resolveTypedTexture = [](std::string_view) {
        return std::optional<dayo::graphics::handles::TextureHandle>{{21, 1}};
    };

    const auto plan = dayo::fx::FxCompiler{}.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext(), resources);
    bool ok = check(stats.raster == 1 && stats.indexedDraws == 2, "raster target selects matching indexed draws");
    ok &= check(std::count_if(commands.trace.begin(), commands.trace.end(),
                              [](const auto& entry) { return entry.starts_with("drawIndexedEx:"); }) == 2,
                "raster executor records one indexed draw per matching material range");
    ok &= check(passModels == std::vector<std::uint32_t>{7, 7},
                "raster executor updates per-draw pass constants before indexed draws");
    ok &= check(dayo::graphics::matchesRasterTarget(dayo::core::fx::RasterModelTarget::self, 7, draws[0]) &&
                    !dayo::graphics::matchesRasterTarget(dayo::core::fx::RasterModelTarget::self, 7, draws[1]) &&
                    dayo::graphics::matchesRasterTarget(dayo::core::fx::RasterModelTarget::other, 7, draws[1]),
                "raster target semantics distinguish self and other models");
    return ok;
}

bool testBufferlessIndexRasterExecution() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    MockCommands commands;

    dayo::fx::FxProgram program;
    dayo::core::EffectBuffer indices;
    indices.name = "Indices";
    indices.elementSize = sizeof(std::uint32_t);
    indices.size.absolute = true;
    indices.size.dimension = 1;
    indices.size.width = 6;
    program.buffers.push_back(indices);

    dayo::fx::FxDispatch dispatch;
    dispatch.name = "particle-bufferless-raster";
    dispatch.kind = dayo::fx::FxOpKind::raster;
    dispatch.resources = {{"Color", true, dayo::fx::FxResourceRole::colorAttachment}};
    dayo::fx::FxRasterDispatch raster;
    raster.vertexShader = "VS";
    raster.pixelShader = "PS";
    raster.graphics.modelTarget = dayo::core::fx::RasterModelTarget::buffer;
    raster.rasterSource = dayo::core::EffectRasterSource::vertexBufferless;
    raster.indexBuffer = "Indices";
    dispatch.executable = raster;
    program.passes.push_back(dispatch);

    dayo::graphics::FxExecutionResources resources;
    resources.resolveTypedPipeline = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::PipelineHandle>{{30, 1}};
    };
    resources.resolveTypedResource =
        [](std::string_view name) -> std::optional<dayo::graphics::FxExecutionResources::TypedResource> {
        if (name == "Indices")
            return dayo::graphics::FxExecutionResources::TypedResource{.buffer = {31, 1}};
        if (name == "Color")
            return dayo::graphics::FxExecutionResources::TypedResource{.texture = {32, 1}};
        return std::nullopt;
    };

    const auto context = testContext();
    const auto plan = dayo::fx::FxCompiler{}.plan(program, context);
    const auto stats = executor.execute(plan, commands, context, resources);
    bool ok = check(plan.resolved[0].raster.has_value() && plan.resolved[0].raster->indexCount == 6,
                    "FX rasterIB resolves its declared element count");
    ok &= check(stats.raster == 1 && stats.indexedDraws == 1, "FX rasterIB counts a vertex-bufferless indexed draw");
    ok &= check(std::ranges::find(commands.trace, "drawIndexedBufferlessEx:6x4") != commands.trace.end(),
                "FX rasterIB records the vertex-bufferless indexed draw with the clone count");
    return ok;
}

bool testFxVertexBufferRasterExecution() {
    using namespace dayo;
    using namespace graphics;
    core::EffectBuffer vertices;
    vertices.name = "VertexData";
    vertices.elementSize = 32;
    vertices.size.absolute = true;
    vertices.size.dimension = 1;
    vertices.size.width = 4;
    core::EffectBuffer indices;
    indices.name = "IndexData";
    indices.elementSize = sizeof(std::uint32_t);
    indices.size.absolute = true;
    indices.size.dimension = 1;
    indices.size.width = 6;

    fx::FxProgram program;
    program.buffers = {vertices, indices};
    fx::FxDispatch dispatch;
    dispatch.name = "particle-buffer-raster";
    dispatch.kind = fx::FxOpKind::raster;
    dispatch.resources = {{"Color", true, fx::FxResourceRole::colorAttachment}};
    fx::FxRasterDispatch raster;
    raster.vertexShader = "VS";
    raster.pixelShader = "PS";
    raster.graphics.modelTarget = core::fx::RasterModelTarget::buffer;
    raster.rasterSource = core::EffectRasterSource::buffer;
    raster.vertexBuffer = "VertexData";
    raster.indexBuffer = "IndexData";
    raster.colorAttachments.push_back({.name = "Color", .clear = false, .clearValue = {}});
    raster.vertexLayout.bindings.push_back({.binding = 0, .stride = 32, .rate = core::EffectVertexInputRate::vertex});
    raster.vertexLayout.attributes.push_back({.location = 0,
                                              .binding = 0,
                                              .format = core::EffectVertexFormat::r32g32b32Float,
                                              .offset = 0,
                                              .semanticName = {},
                                              .semanticIndex = 0,
                                              .formatName = {}});
    raster.vertexLayout.attributes.push_back({.location = 1,
                                              .binding = 0,
                                              .format = core::EffectVertexFormat::r32g32Float,
                                              .offset = 12,
                                              .semanticName = {},
                                              .semanticIndex = 0,
                                              .formatName = {}});
    dispatch.executable = raster;
    program.passes.push_back(dispatch);

    FxExecutionResources resources;
    resources.resolveTypedPipeline = [](const fx::FxDispatch&) {
        return std::optional<handles::PipelineHandle>{{30, 1}};
    };
    resources.resolveTypedTexture = [](std::string_view name) {
        return name == "Color" ? std::optional<handles::TextureHandle>{{43, 1}} : std::nullopt;
    };
    resources.resolveTypedResource = [](std::string_view name) -> std::optional<FxExecutionResources::TypedResource> {
        if (name == "VertexData")
            return FxExecutionResources::TypedResource{.buffer = {41, 1}};
        if (name == "IndexData")
            return FxExecutionResources::TypedResource{.buffer = {42, 1}};
        if (name == "Color")
            return FxExecutionResources::TypedResource{.texture = {43, 1}};
        return std::nullopt;
    };

    const auto context = testContext();
    const auto plan = fx::FxCompiler{}.plan(program, context);
    MockCommands commands;
    MockDevice device;
    const auto stats = VulkanFxExecutor(device).execute(plan, commands, context, resources);
    bool ok = check(plan.resolved[0].raster.has_value() && plan.resolved[0].raster->vertexCount == 4 &&
                        plan.resolved[0].raster->indexCount == 6,
                    "FX rasterVB/rasterIB resolve their declared element counts");
    ok &= check(stats.raster == 1 && stats.indexedDraws == 1 && commands.indexedDraws.size() == 1,
                "FX vertex/index buffer raster emits a typed indexed draw");
    if (!commands.indexedDraws.empty()) {
        const auto& draw = commands.indexedDraws.front();
        ok &= check(draw.vertexBuffer == handles::BufferHandle{41, 1} &&
                        draw.indexBuffer == handles::BufferHandle{42, 1} && draw.indexCount == 6 &&
                        draw.instanceCount == context.cloneCount && draw.vertexBuffers.size() == 1 &&
                        draw.vertexBuffers.front().binding == 0,
                    "FX indexed buffer raster binds its vertex stream and clone instances");
    }

    raster.indexBuffer.clear();
    dispatch.executable = raster;
    program.passes = {dispatch};
    const auto vertexOnlyPlan = fx::FxCompiler{}.plan(program, context);
    MockCommands vertexOnlyCommands;
    const auto vertexOnlyStats =
        VulkanFxExecutor(device).execute(vertexOnlyPlan, vertexOnlyCommands, context, resources);
    ok &= check(vertexOnlyStats.vertexBufferDraws == 1 && vertexOnlyCommands.vertexBufferDraws.size() == 1 &&
                    vertexOnlyCommands.vertexBufferDraws.front().vertexCount == 4 &&
                    vertexOnlyCommands.vertexBufferDraws.front().instanceCount == context.cloneCount,
                "FX rasterVB without rasterIB emits a typed vertex-buffer draw");

    auto layoutRaster = raster;
    layoutRaster.vertexLayout.bindings.push_back(
        {.binding = 1, .stride = 16, .rate = core::EffectVertexInputRate::instance});
    layoutRaster.vertexLayout.attributes.push_back({.location = 2,
                                                    .binding = 1,
                                                    .format = core::EffectVertexFormat::r32g32b32a32Float,
                                                    .offset = 0,
                                                    .semanticName = {},
                                                    .semanticIndex = 0,
                                                    .formatName = {}});
    auto layoutDispatch = dispatch;
    layoutDispatch.executable = layoutRaster;
    const auto pipeline = makeGraphicsPipelineDescriptor(program, layoutDispatch, {7, 1}, {});
    ok &= check(pipeline.vertexBindings.size() == 2 && pipeline.vertexBindings[0].stride == 32 &&
                    pipeline.vertexBindings[1].rate == VertexInputRateEx::instance &&
                    pipeline.vertexAttributes.size() == 3 &&
                    pipeline.vertexAttributes[0].format == VertexInputFormatEx::r32g32b32Sfloat &&
                    pipeline.vertexAttributes[1].format == VertexInputFormatEx::r32g32Sfloat &&
                    pipeline.vertexAttributes[1].offset == 12 &&
                    pipeline.vertexAttributes[2].format == VertexInputFormatEx::r32g32b32a32Sfloat,
                "FX vertex layout maps to backend-neutral pipeline input descriptions");
    auto invalidRaster = layoutRaster;
    invalidRaster.vertexLayout.attributes.front().offset = 24;
    dispatch.executable = invalidRaster;
    bool invalidLayoutRejected = false;
    try {
        static_cast<void>(makeGraphicsPipelineDescriptor(program, dispatch, {7, 1}, {}));
    } catch (const std::invalid_argument&) {
        invalidLayoutRejected = true;
    }
    ok &= check(invalidLayoutRejected, "FX vertex input attributes cannot exceed their declared stride");
    return ok;
}

bool testDepthOnlyRasterExecution() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    MockCommands commands;
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "depth-only";
    dispatch.kind = dayo::fx::FxOpKind::raster;
    dayo::fx::FxRasterDispatch raster;
    raster.vertexShader = "VS";
    raster.depthAttachment = dayo::core::EffectAttachment{.name = "Depth", .clear = true, .clearValue = {}};
    dispatch.executable = raster;
    dispatch.resources = {{"Depth", true, dayo::fx::FxResourceRole::depthAttachment}};
    dayo::fx::FxProgram program;
    program.passes.push_back(dispatch);
    dayo::graphics::FxExecutionResources resources;
    resources.resolveTypedPipeline = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::PipelineHandle>{{20, 1}};
    };
    resources.resolveTypedTexture = [](std::string_view name) -> std::optional<dayo::graphics::handles::TextureHandle> {
        return name == "Depth" ? std::optional<dayo::graphics::handles::TextureHandle>{{21, 1}} : std::nullopt;
    };
    const auto plan = dayo::fx::FxCompiler{}.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext(), resources);
    return check(stats.raster == 1 &&
                     std::ranges::find(commands.trace, "beginRenderingInfoEx:0") != commands.trace.end() &&
                     std::ranges::find(commands.trace, "beginRenderingEx") == commands.trace.end(),
                 "depth-only raster pass begins typed rendering with zero color attachments");
}
bool testOidnHostExecution() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    dayo::fx::FxProgram program;
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "oidn-pass";
    dispatch.kind = dayo::fx::FxOpKind::oidn;
    dispatch.executable =
        dayo::fx::FxOidnDispatch{.input = "Beauty", .albedo = "Albedo", .normal = "Normal", .output = "Denoised"};
    dispatch.resources = {{"Beauty", false}, {"Albedo", false}, {"Normal", false}, {"Denoised", true}};
    dispatch.resources[3].role = dayo::fx::FxResourceRole::storage;
    program.passes.push_back(dispatch);

    std::vector<std::string> executedResources;
    dayo::graphics::FxExecutionResources resources;
    resources.resolveTypedResource =
        [](std::string_view name) -> std::optional<dayo::graphics::FxExecutionResources::TypedResource> {
        if (name == "Beauty")
            return dayo::graphics::FxExecutionResources::TypedResource{.texture = {1, 1}};
        if (name == "Albedo")
            return dayo::graphics::FxExecutionResources::TypedResource{.texture = {2, 1}};
        if (name == "Normal")
            return dayo::graphics::FxExecutionResources::TypedResource{.texture = {3, 1}};
        if (name == "Denoised")
            return dayo::graphics::FxExecutionResources::TypedResource{.texture = {4, 1}};
        return std::nullopt;
    };
    resources.executeOidn = [&executedResources](const dayo::fx::FxOidnDispatch& oidn, const dayo::fx::FxFrameContext&,
                                                 dayo::graphics::CommandList&) {
        executedResources = {oidn.input, oidn.albedo, oidn.normal, oidn.output};
        return true;
    };
    MockCommands commands;
    const auto plan = dayo::fx::FxCompiler{}.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext(), resources);
    bool ok = check(stats.oidn == 1 &&
                        executedResources == std::vector<std::string>{"Beauty", "Albedo", "Normal", "Denoised"},
                    "OIDN dispatch forwards upstream resource semantics to the host");
    ok &= check(std::count(commands.trace.begin(), commands.trace.end(), "transitionEx") == 4,
                "OIDN dispatch validates and transitions every typed image resource");

    auto missingHost = resources;
    missingHost.executeOidn = {};
    bool rejected = false;
    try {
        static_cast<void>(executor.execute(plan, commands, testContext(), missingHost));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    ok &= check(rejected, "OIDN dispatch fails explicitly when no host denoiser is installed");
    return ok;
}

bool testOidnStructuredBufferInput() {
    MockDevice device;
    const std::array<dayo::graphics::NativeSceneOidnInput, 2> inputSamples = {
        dayo::graphics::NativeSceneOidnInput{
            .color = {0.25F, 0.5F, 0.75F}, .albedo = {0.1F, 0.2F, 0.3F}, .normal = {0.4F, 0.5F, 0.6F}},
        dayo::graphics::NativeSceneOidnInput{
            .color = {1.0F, 0.75F, 0.5F}, .albedo = {0.6F, 0.7F, 0.8F}, .normal = {0.9F, 1.0F, 0.1F}},
    };
    const auto inputBytes = std::as_bytes(std::span(inputSamples));
    device.bufferReadbackBytes_.assign(inputBytes.begin(), inputBytes.end());

    dayo::graphics::NativeOidnProvider provider(device);
    auto context = testContext();
    context.renderWidth = 2;
    context.renderHeight = 1;
    const dayo::fx::FxOidnDispatch dispatch{.input = "OIDNBuf", .albedo = "", .normal = "", .output = "Denoised"};
    const dayo::graphics::FxExecutionResources::TypedResourceResolver resolve =
        [](std::string_view name) -> std::optional<dayo::graphics::FxExecutionResources::TypedResource> {
        if (name == "OIDNBuf")
            return dayo::graphics::FxExecutionResources::TypedResource{.buffer = {31, 1}};
        if (name == "Denoised")
            return dayo::graphics::FxExecutionResources::TypedResource{.texture = {32, 1}};
        return std::nullopt;
    };
    MockCommands commands;
    std::string error;
    const auto executed = provider.execute(dispatch, context, commands, resolve, &error);
    bool ok = check(executed, "OIDN host accepts NativeSceneOidnInput structured-buffer input");
    ok &= check(device.lastBufferReadbackHandle_ == dayo::graphics::handles::BufferHandle{31, 1} &&
                    device.lastBufferReadbackOffset_ == 0 &&
                    device.lastBufferReadbackSize_ == inputSamples.size() * sizeof(inputSamples.front()),
                "OIDN host reads exactly the interleaved scene input buffer extent");
    ok &= check(std::ranges::find(commands.trace, "flushAndWaitForHostReadbackEx") != commands.trace.end() &&
                    device.lastUploadedTexture_ == dayo::graphics::handles::TextureHandle{32, 1} &&
                    device.uploadedTextureBytes_ == inputSamples.size() * 8U,
                "OIDN buffer path flushes GPU writes and uploads the processed output texture");
    if (provider.runtime().lastPath() == dayo::core::DenoiserPath::passthrough) {
        const auto firstRed = static_cast<std::uint16_t>(device.uploadedTextureData_[0]) |
                              static_cast<std::uint16_t>(device.uploadedTextureData_[1] << 8U);
        const auto secondRed = static_cast<std::uint16_t>(device.uploadedTextureData_[8]) |
                               static_cast<std::uint16_t>(device.uploadedTextureData_[9] << 8U);
        ok &= check(firstRed == 0x3400U && secondRed == 0x3c00U,
                    "OIDN unavailable passthrough preserves the structured buffer beauty channel");
    }
    return ok;
}

bool testTypedBufferResourceExecution() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    dayo::fx::FxProgram program;
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "buffer-pass";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.resources.push_back({"Lights", true});
    program.passes.push_back(dispatch);

    dayo::graphics::FxExecutionResources resources;
    resources.resolveTypedResource =
        [](std::string_view name) -> std::optional<dayo::graphics::FxExecutionResources::TypedResource> {
        if (name != "Lights")
            return std::nullopt;
        return dayo::graphics::FxExecutionResources::TypedResource{
            .buffer = {7, 1},
        };
    };
    resources.resolveTypedPipeline = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::PipelineHandle>{{8, 1}};
    };
    resources.resolveDescriptorSet = [](const dayo::fx::FxDispatch&) {
        return std::optional<dayo::graphics::handles::DescriptorSetHandle>{{9, 1}};
    };
    MockCommands commands;
    const auto plan = dayo::fx::FxCompiler{}.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext(), resources);
    bool ok = check(stats.compute == 1, "executor runs a buffer-only typed pass");
    ok &=
        check(commands.trace == std::vector<std::string>{"descriptorEx", "bindEx", "dispatch:4x4x1", "memoryBarrierEx"},
              "buffer-only typed pass skips image transitions");
    return ok;
}

bool testBufferClearUsesFunctionalValue() {
    MockDevice device;
    dayo::graphics::VulkanFxExecutor executor(device);
    dayo::fx::FxProgram program;
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "clear-buffer";
    dispatch.kind = dayo::fx::FxOpKind::clear;
    dispatch.functionalKind = dayo::core::EffectFunctionalPassKind::clearUav;
    dispatch.functional.kind = dayo::core::EffectFunctionalPassKind::clearUav;
    dispatch.functional.clearValue.color = {1.0F, 2.0F, 3.0F, 4.0F};
    dispatch.resources.push_back({"Values", true, dayo::fx::FxResourceRole::storage});
    program.passes.push_back(dispatch);

    dayo::graphics::FxExecutionResources resources;
    resources.resolveTypedResource =
        [](std::string_view name) -> std::optional<dayo::graphics::FxExecutionResources::TypedResource> {
        if (name != "Values")
            return std::nullopt;
        return dayo::graphics::FxExecutionResources::TypedResource{.buffer = {7, 1}};
    };
    MockCommands commands;
    const auto plan = dayo::fx::FxCompiler{}.plan(program, testContext());
    const auto stats = executor.execute(plan, commands, testContext(), resources);
    return check(stats.clear == 1 && commands.trace.size() == 1 &&
                     commands.trace.front() == "clearBufferEx:1.000000:2.000000:3.000000:4.000000",
                 "buffer clear dispatch forwards the functional pass value instead of forcing zero");
}

bool testDayoHostResourceProvider() {
    dayo::graphics::NativeSceneResourceBindings bindings;
    bindings.rtOutput = {1, 1};
    bindings.screenBmp = {2, 1};
    bindings.viewConstants = {3, 1};
    bindings.controllerConstants = {4, 1};
    bindings.hostResourceMask = dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::RTOutput) |
                                dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::ScreenBMP) |
                                dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::ViewCB) |
                                dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::ControllerCB);
    const dayo::graphics::DayoHostResourceProvider provider(bindings);
    bool ok = true;
    dayo::graphics::DayoSceneHostProvider sceneProvider(provider);
    ok &= check(sceneProvider.supports("RTOutput") && sceneProvider.resolve("RTOutput", testContext()).texture.valid(),
                "generic Dayo runtime accepts the canonical scene provider");
    dayo::graphics::DayoFxRuntime genericRuntime;
    genericRuntime.addProvider(sceneProvider);
    ok &= check(genericRuntime.providerCount() == 1, "generic Dayo runtime owns provider registration only");
    const std::array required{dayo::graphics::DayoSemantic::RTOutput, dayo::graphics::DayoSemantic::ViewCB,
                              dayo::graphics::DayoSemantic::ControllerCB};
    std::string error;
    ok &= check(provider.require(required, &error) && error.empty(), "host provider requires real upstream semantics");
    ok &= check(provider.resolve("YRZFX_ControllerCB").has_value(),
                "host provider accepts canonical controller binding alias");
    const std::array missing{dayo::graphics::DayoSemantic::GBuffer1};
    ok &= check(!provider.require(missing, &error) && error.find("GBuffer1") != std::string::npos,
                "host provider rejects absent semantics instead of returning placeholders");
    auto placeholderOnly = bindings;
    placeholderOnly.hostResourceMask = 0;
    ok &= check(!dayo::graphics::DayoHostResourceProvider(placeholderOnly)
                     .resolve(dayo::graphics::DayoSemantic::RTOutput)
                     .has_value(),
                "valid placeholder handles do not satisfy an unmarked semantic");
    return ok;
}

bool testNativeSceneDerivedResources() {
    using dayo::graphics::NativeSceneDerivedModel;
    const std::array models{
        NativeSceneDerivedModel{
            .materialCount = 2, .textureBase = 12, .cloneCount = 3, .selectedMaterial = 1, .visible = true},
        NativeSceneDerivedModel{
            .materialCount = 1, .textureBase = 20, .cloneCount = 0, .selectedMaterial = -1, .visible = false}};
    const auto data = dayo::graphics::makeNativeSceneDerivedData(models);
    bool ok = check(data.modelToMaterial == std::vector<std::array<std::uint32_t, 2>>{{0, 2}, {2, 1}},
                    "Model2Mat preserves material prefix ranges");
    ok &= check(data.materialToModel == std::vector<std::uint32_t>{0, 0, 1}, "Mat2Model follows scene model order");
    ok &= check(data.peekaboo == std::vector<std::int32_t>{1, 0}, "Peekaboo preserves model visibility");
    ok &= check(data.materialSelected == std::vector<std::int32_t>{0, 1, 0},
                "MatSelected marks the selected local material");
    ok &= check(data.cloneCount == std::vector<std::uint32_t>{3, 1}, "CloneCount is clamped to at least one");
    ok &= check(data.textureTable == std::vector<std::uint32_t>{12, 20},
                "TextureTable preserves flattened per-model offsets");

    const auto empty = dayo::graphics::makeNativeSceneDerivedData({});
    ok &= check(empty.modelToMaterial.size() == 1 && empty.materialToModel.size() == 1 &&
                    empty.cloneCount == std::vector<std::uint32_t>{1},
                "empty scenes retain valid one-element structured-buffer data");

    MockDevice device;
    std::vector<dayo::core::ImageRgba8> images;
    images.push_back({2, 1, std::vector<std::uint8_t>(8, 127)});
    images.emplace_back();
    dayo::graphics::NativeSceneDerivedRuntime runtime;
    std::string error;
    ok &= check(runtime.initialize(device, images, &error) && error.empty(),
                "scene-derived runtime uploads real and fallback PMX textures");
    ok &= check(runtime.textures().size() == 2 && runtime.textures()[0] != runtime.textures()[1] &&
                    device.textureUploads_ == 2,
                "missing PMX image slots keep their table index through a white fallback");
    ok &= check(runtime.sync(models, {8, 4, 1}, &error) && error.empty() && device.bufferUploads_.size() == 6,
                "scene-derived runtime uploads lookup tables and allocates output resources");
    ok &= check(device.textureDescs_.size() >= 6 &&
                    device.textureDescs_.back().format == dayo::graphics::PixelFormat::r32g32Float &&
                    device.textureDescs_.back().extent.width == 8 && device.textureDescs_.back().extent.height == 4,
                "size-dependent GBuffer resources use the requested output extent and typed format");
    ok &= check(device.bufferClears_ == dayo::graphics::kNativeFramesInFlight,
                "new OIDN buffers are initialized before the first effect invocation");
    if (!device.bufferUploads_.empty()) {
        std::array<std::uint32_t, 2> firstModelRange{};
        const auto& bytes = device.bufferUploads_.front().bytes;
        if (bytes.size() >= sizeof(firstModelRange))
            std::memcpy(firstModelRange.data(), bytes.data(), sizeof(firstModelRange));
        ok &=
            check(firstModelRange == std::array<std::uint32_t, 2>{0, 2}, "GPU Model2Mat upload matches the CPU table");
    } else {
        ok &= check(false, "GPU Model2Mat upload was recorded");
    }
    dayo::graphics::NativeSceneResourceBindings bindings;
    runtime.apply(bindings);
    ok &= check(bindings.modelToMaterial.valid() && bindings.materialToModel.valid() && bindings.cloneCount.valid() &&
                    bindings.textureTable.valid() && bindings.textures.size() == 2 && bindings.rtOutput.valid() &&
                    bindings.oidnBuffer.valid() && bindings.normalDepth.valid() && bindings.gbuffer1.valid() &&
                    bindings.gbuffer2.valid(),
                "derived GPU handles connect to canonical scene bindings");
    const auto previousTextureCount = device.textureDescs_.size();
    ok &= check(runtime.sync(models, {16, 8, 1}, &error) && error.empty() &&
                    device.textureDescs_.size() == previousTextureCount + 4 * dayo::graphics::kNativeFramesInFlight,
                "render-size changes recreate the scene output set for each in-flight frame");
    runtime.reset();
    ok &= check(device.destroyedTextures_ == device.textureDescs_.size(),
                "scene-derived runtime releases owned PMX, fallback, and output textures");
    return ok;
}

bool testViewConstantsAndScreenHistory() {
    MockDevice device;
    auto context = testContext();
    context.camera.view[0] = 2.0F;
    context.camera.projection[5] = 3.0F;
    context.sample = 3;
    context.sampleCount = 8;
    context.host.selfShadowMode = 2;
    context.host.selfShadowDistance = 4.0F;
    context.host.screenBmpMode = 1;
    context.host.backgroundMode = 3;
    context.host.backgroundTransparent = true;
    context.host.playing = true;
    context.host.denoiserEnabled = true;
    context.host.onResize = true;
    context.modelCount = 4;
    context.cloneCount = 9;
    const auto view = dayo::graphics::makeNativeViewConstants(context, 8);
    bool ok = check(view.viewMatrix[0] == 2.0F && view.projectionMatrix[5] == 3.0F && view.modelCounts[0] == 4 &&
                        view.modelCounts[1] == 8,
                    "ViewCB uses frame camera matrices");
    ok &= check(view.output[2] == 3 && view.output[3] == 8,
                "ViewCB carries zero-based sample index and samples per frame");
    ok &= check(view.selfShadowMode == 2 && view.selfShadowDistance == 4.0F && view.screenBmpMode == 1 &&
                    view.backgroundMode == 3 && view.backgroundTransparent == 1 && view.denoiserEnabled == 1 &&
                    view.playing == 1 && view.onResize == 1,
                "ViewCB carries upstream host frame flags");

    auto expressionContext = context;
    expressionContext.expressionSymbols.emplace("Exposure", 0.75);
    auto expressionView = view;
    expressionView.frameTimes = {12.5F, 0.25F, 10.0F, 0.5F};
    expressionView.realTimes = {99.0F, 0.75F};
    expressionView.output = {64, 32, 3, 8};
    expressionView.cameraFlags = {1, 1};
    dayo::graphics::populateNativeViewExpressionSymbols(expressionContext, expressionView);
    dayo::core::fx::FxEvalContext evalContext;
    evalContext.time = expressionContext.time;
    evalContext.namedSymbols = expressionContext.expressionSymbols;
    ok &= check(dayo::core::fx::fxToDouble(
                    dayo::core::fx::evaluateFxExpr(dayo::core::fx::parseFxExpr("Time"), evalContext)) == 12.5,
                "Time expression reads the same ViewCB slot uploaded to HLSL");
    ok &= check(dayo::core::fx::fxToDouble(dayo::core::fx::evaluateFxExpr(
                    dayo::core::fx::parseFxExpr("DTime + FrameTime + DFrameTime + RealTime + DRealTime"),
                    evalContext)) == 110.5,
                "cb.hlsli time symbols resolve from the uploaded ViewCB payload");
    ok &= check(dayo::core::fx::fxToDouble(dayo::core::fx::evaluateFxExpr(
                    dayo::core::fx::parseFxExpr("Exposure + Resolution.x + Resolution.y + iSample + SamplesPerFrame"),
                    evalContext)) == 107.75,
                "application-provided host symbols coexist with cb.hlsli expression symbols");
    ok &= check(
        dayo::core::fx::fxToDouble(dayo::core::fx::evaluateFxExpr(
            dayo::core::fx::parseFxExpr("Perspective + CameraInterpolated + Playing + OnResize"), evalContext)) == 4.0,
        "ViewCB scalar flags are available to runtime expressions");

    dayo::graphics::NativeScreenRuntime screen;
    std::string error;
    ok &= check(screen.initialize(device, {64, 32, 1}, &error) && screen.ready() && error.empty(),
                "screen runtime allocates persistent logical resources");
    const auto screenBmp = screen.screenBmp();
    const auto screenTexture = screen.screenTexture();
    const auto previousFrame = screen.previousFrame();
    ok &= check(screenBmp.valid() && screenTexture.valid() && previousFrame.valid() && screenBmp != screenTexture &&
                    screenTexture != previousFrame,
                "ScreenBMP ScreenTexture and PreviousFrame stay distinct");
    dayo::graphics::NativeSceneResourceBindings bindings;
    screen.bindScreenSemantics(bindings);
    ok &= check(
        bindings.screenBmp == screenBmp && bindings.screenTexture == screenTexture &&
            (bindings.hostResourceMask & dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::ScreenBMP)) !=
                0 &&
            bindings.rtOutput == previousFrame &&
            (bindings.hostResourceMask & dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::RTOutput)) != 0,
        "screen runtime exposes strict ScreenBMP semantics");
    MockCommands commands;
    screen.rotatePreviousFrame(commands, {99, 1});
    ok &= check(commands.trace == std::vector<std::string>{"transferBarrierEx", "copyEx"},
                "screen runtime rotates final output into persistent history");
    commands.trace.clear();
    screen.prepareFrame(commands, dayo::graphics::NativeScreenSource::previousFrame, true,
                        dayo::graphics::NativeScreenCrop::crop4x3);
    ok &= check(commands.trace == std::vector<std::string>{"transferBarrierEx", "copyEx", "blitEx:11:0:53:32"},
                "previous-frame ScreenBMP applies the same 4:3 crop as external backgrounds");
    return ok;
}

bool testFxControllerResolver() {
    dayo::core::fx::SceneEvaluationSnapshot snapshot;
    dayo::core::fx::EvaluatedModelState model;
    model.id = 11;
    model.sourcePath = "ToonAnime.pmx";
    model.displayName = "Toon Anime";
    model.modelName = "ToonAnime";
    model.morphNames = {"Smile"};
    model.morphWeights = {0.75F};
    model.boneNames = {"arm"};
    model.bones.push_back({.rotation = {0.0F, 0.0F, 0.0F, 1.0F}, .translation = {1.0F, 2.0F, 3.0F}});
    snapshot.models.push_back(model);
    dayo::core::fx::FxControllerResolver resolver;
    const auto morph = resolver.resolve(
        {.name = "Gain", .controllerName = "(self)", .item = "Smile", .type = "float", .descriptions = {}}, snapshot,
        11);
    const auto bone = resolver.resolve(
        {.name = "Position", .controllerName = "ToonAnime.pmx", .item = "arm", .type = "float3", .descriptions = {}},
        snapshot, 11);
    bool ok = check(std::get<float>(morph) == 0.75F, "controller resolver reads evaluated morph weight");
    ok &= check(std::get<std::array<float, 3>>(bone) == std::array<float, 3>{1.0F, 2.0F, 3.0F},
                "controller resolver reads evaluated bone translation");

    dayo::core::SceneEffectStack effectStack;
    dayo::core::SceneEffectInstance effect;
    effect.controllerModel = 11;
    effect.graph.controllers.push_back({.name = "SmileWeight",
                                        .controllerName = "(self)",
                                        .item = "Smile",
                                        .type = "float",
                                        .slider = dayo::core::EffectSlider{.minimum = -2.0F,
                                                                           .maximum = 3.0F,
                                                                           .step = 0.25F,
                                                                           .defaultValue = 1.0F,
                                                                           .logarithmic = true,
                                                                           .integer = false},
                                        .description = "Smile amount",
                                        .descriptions = {"Smile amount", "笑顔の強さ"}});
    effectStack.deform.push_back(std::move(effect));
    dayo::core::ModelInstance uiModel;
    uiModel.id = 11;
    const auto uiMetadata = dayo::core::fx::resolveFxMorphControllerUi(effectStack, uiModel, "Smile");
    ok &= check(uiMetadata.has_value() && uiMetadata->slider.has_value() && uiMetadata->slider->minimum == -2.0F &&
                    uiMetadata->slider->maximum == 3.0F && uiMetadata->slider->step == 0.25F &&
                    uiMetadata->slider->defaultValue == 1.0F && uiMetadata->slider->logarithmic &&
                    uiMetadata->descriptions == std::vector<std::string>{"Smile amount", "笑顔の強さ"},
                "morph controller UI resolver preserves slider and localized descriptions");

    snapshot.models.front().bones.front().rotation = {0.0F, 0.0F, 0.70710677F, 0.70710677F};
    const auto boneMatrix = resolver.resolve(
        {.name = "Transform", .controllerName = "(self)", .item = "arm", .type = "float4x4", .descriptions = {}},
        snapshot, 11);
    const auto& matrix = std::get<std::array<float, 16>>(boneMatrix);
    ok &= check(std::abs(matrix[1] - 1.0F) < 0.0001F && std::abs(matrix[4] + 1.0F) < 0.0001F && matrix[12] == 1.0F &&
                    matrix[13] == 2.0F && matrix[14] == 3.0F,
                "controller resolver preserves bone quaternion rotation in float4x4");
    auto duplicate = model;
    duplicate.id = 12;
    duplicate.morphWeights = {0.25F};
    snapshot.models.push_back(duplicate);
    std::string controllerError;
    const std::array arrayController{dayo::core::EffectController{.name = "Exposure[2]",
                                                                  .controllerName = "ToonAnime.pmx",
                                                                  .item = "Smile",
                                                                  .type = "float",
                                                                  .descriptions = {}}};
    dayo::graphics::NativeControllerBlock arrayBlock(dayo::graphics::makeNativeControllerLayout(arrayController));
    const bool arrayResolved =
        dayo::graphics::resolveNativeControllerBlock(arrayBlock, arrayController, snapshot, 11, &controllerError);
    const auto* exposureField = arrayBlock.layout().find("Exposure[2]");
    std::array<float, 2> exposureValues{};
    if (exposureField != nullptr) {
        std::memcpy(exposureValues.data(), arrayBlock.bytes().data() + exposureField->offset, sizeof(float));
        std::memcpy(&exposureValues[1],
                    arrayBlock.bytes().data() + exposureField->offset + exposureField->elementStride, sizeof(float));
    }
    ok &= check(arrayResolved && exposureValues == std::array<float, 2>{0.75F, 0.25F},
                "controller arrays resolve same-name PMX models in scene order");
    const std::array effectControllers{dayo::core::EffectController{
        .name = "Exposure", .controllerName = "(self)", .item = "Smile", .type = "float", .descriptions = {}}};
    dayo::graphics::NativeControllerBlock first(dayo::graphics::makeNativeControllerLayout(effectControllers));
    dayo::graphics::NativeControllerBlock second(dayo::graphics::makeNativeControllerLayout(effectControllers));
    ok &= check(
        dayo::graphics::resolveNativeControllerBlock(first, effectControllers, snapshot, 11, &controllerError) &&
            dayo::graphics::resolveNativeControllerBlock(second, effectControllers, snapshot, 12, &controllerError) &&
            !std::ranges::equal(first.bytes(), second.bytes()),
        "identical controller names resolve independently for two effect owners");
    dayo::graphics::NativeSceneResourceBindings firstBindings;
    firstBindings.controllerConstants = {101, 1};
    firstBindings.globalConstants = {201, 1};
    auto secondBindings = firstBindings;
    secondBindings.controllerConstants = {102, 1};
    secondBindings.globalConstants = {202, 1};
    const auto firstFrame = dayo::graphics::nativeSceneFrameDescriptorBindings(firstBindings);
    const auto secondFrame = dayo::graphics::nativeSceneFrameDescriptorBindings(secondBindings);
    const auto controllerSlot =
        dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uniform, 1);
    const auto firstDescriptor = std::ranges::find_if(
        firstFrame, [controllerSlot](const auto& binding) { return binding.slot == controllerSlot; });
    const auto secondDescriptor = std::ranges::find_if(
        secondFrame, [controllerSlot](const auto& binding) { return binding.slot == controllerSlot; });
    ok &= check(firstDescriptor != firstFrame.end() && secondDescriptor != secondFrame.end() &&
                    firstDescriptor->buffer != secondDescriptor->buffer,
                "effect-local frame descriptor sets bind distinct ControllerCB buffers");
    const auto globalSlot = dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uniform, 2);
    const auto firstGlobal =
        std::ranges::find_if(firstFrame, [globalSlot](const auto& binding) { return binding.slot == globalSlot; });
    const auto secondGlobal =
        std::ranges::find_if(secondFrame, [globalSlot](const auto& binding) { return binding.slot == globalSlot; });
    ok &= check(firstGlobal != firstFrame.end() && secondGlobal != secondFrame.end() &&
                    firstGlobal->buffer == dayo::graphics::handles::BufferHandle{201, 1} &&
                    secondGlobal->buffer == dayo::graphics::handles::BufferHandle{202, 1},
                "YRZFX global constants occupy the upstream b2 descriptor and remain effect-local");
    ok &= check(
        [&] {
            try {
                static_cast<void>(resolver.resolve({.name = "Ambiguous",
                                                    .controllerName = "ToonAnime.pmx",
                                                    .item = "Smile",
                                                    .type = "float",
                                                    .descriptions = {}},
                                                   snapshot, 11));
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        }(),
        "controller resolver rejects ambiguous model targets");
    return ok;
}

bool testFxConditionRuntime() {
    dayo::fx::FxConditionRuntime runtime;
    dayo::core::fx::FakeFxResourceTable resourceTable;
    resourceTable.add("Probe", {.x = 4, .y = 2, .z = 1, .dimension = 2});
    auto context = testContext();
    context.frame = 5.75F;
    context.time = 0.75;
    context.expressionSymbols.emplace("Exposure", 0.75);
    context.host.onResize = true;
    context.host.onModelChanged = true;
    const std::vector<std::string> conditions = {"frame if FRAME >= 5", "resize if DEFAULT_RTSIZE.x == 64",
                                                 "modelChanged if Probe.x == 4", "frame if frac(Time) >= 0.5",
                                                 "frame if Exposure >= 0.5"};
    bool ok = check(runtime.evaluate(conditions, context, &resourceTable),
                    "condition runtime combines frame events, predicates, and resource extents");
    context.expressionSymbols.insert_or_assign("Exposure", 0.25);
    ok &= check(!runtime.evaluate(conditions, context, &resourceTable),
                "condition runtime resolves named constant-buffer values from the frame context");
    context.expressionSymbols.insert_or_assign("Exposure", 0.75);
    context.frame = 4.0F;
    ok &= check(!runtime.evaluate(conditions, context, &resourceTable),
                "condition runtime rejects a false expression predicate");
    context.frame = 5.0F;
    context.host.onResize = false;
    ok &= check(!runtime.evaluate(conditions, context, &resourceTable), "condition runtime rejects an inactive event");
    ok &= check(runtime.evaluate({}, context), "empty condition list is unconditional");
    return ok;
}

bool testNativeFxGlobalVariableRuntime() {
    MockDevice device;
    device.capabilities_.maxUniformBufferRange = 4096;
    dayo::graphics::NativeFxGlobalVariableRuntime runtime;
    std::string error;
    bool ok = check(runtime.initialize(device, 64, &error) && error.empty() && runtime.ready() && runtime.size() == 64,
                    "native FX global-variable runtime allocates the declared byte range");
    ok &= check(runtime.buffer().valid() && !device.bufferDescs_.empty() && device.bufferDescs_.back().size == 64 &&
                    device.bufferDescs_.back().cpuVisible &&
                    device.bufferDescs_.back().lifetime == dayo::graphics::ResourceLifetime::persistent &&
                    (dayo::graphics::toBits(device.bufferDescs_.back().usage) &
                     dayo::graphics::toBits(dayo::graphics::ResourceUsage::uniformRead)) != 0,
                "native FX global-variable allocation is a persistent uniform buffer");
    ok &= check(device.bufferUploads_.size() == 1 && device.bufferUploads_.back().bytes.size() == 64 &&
                    std::ranges::all_of(device.bufferUploads_.back().bytes,
                                        [](std::byte value) { return value == std::byte{0}; }),
                "native FX global-variable buffer begins deterministically zeroed");
    runtime.reset();
    device.capabilities_.maxUniformBufferRange = 32;
    ok &= check(!runtime.initialize(device, 64, &error) && !error.empty() && !runtime.buffer().valid(),
                "native FX global-variable allocation rejects ranges unsupported by the device");
    ok &= check(!runtime.initialize(device, dayo::graphics::kMaxNativeFxGlobalVariableBytes + 1U, &error) &&
                    error.find("64 KiB") != std::string::npos,
                "native FX global-variable allocation enforces the constant-buffer size ceiling");
    ok &= check(runtime.initialize(device, 0, &error) && runtime.ready() && !runtime.buffer().valid(),
                "explicit zero global-variable size disables the allocation");
    return ok;
}

bool testFxMaterialGpuRuntimeOwnsTablesAndTextures() {
    namespace fs = std::filesystem;
    const auto directory =
        fs::temp_directory_path() /
        ("dayo-matdesc-gpu-runtime-" +
         std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code filesystemError;
    fs::create_directories(directory, filesystemError);
    if (filesystemError)
        return check(false, "MatDesc GPU runtime test directory created");

    const auto imagePath = directory / "albedo.ppm";
    {
        std::ofstream output(imagePath, std::ios::binary);
        output << "P6\n2 1\n255\n";
        output.put(static_cast<char>(255));
        output.put(static_cast<char>(0));
        output.put(static_cast<char>(0));
        output.put(static_cast<char>(0));
        output.put(static_cast<char>(255));
        output.put(static_cast<char>(0));
    }

    const auto volumePath = directory / "volume.dds";
    std::array<std::uint8_t, 148> volumeHeader{};
    const auto putVolumeHeader = [&volumeHeader](std::size_t offset, std::uint32_t value) {
        volumeHeader[offset] = static_cast<std::uint8_t>(value);
        volumeHeader[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
        volumeHeader[offset + 2] = static_cast<std::uint8_t>(value >> 16U);
        volumeHeader[offset + 3] = static_cast<std::uint8_t>(value >> 24U);
    };
    std::copy_n("DDS ", 4, volumeHeader.begin());
    putVolumeHeader(4, 124);
    putVolumeHeader(12, 1);
    putVolumeHeader(16, 2);
    putVolumeHeader(24, 2);
    putVolumeHeader(28, 2);
    putVolumeHeader(76, 32);
    putVolumeHeader(80, 4);
    putVolumeHeader(84, 0x30315844); // DX10
    putVolumeHeader(108, 0x1000);
    putVolumeHeader(112, 0x200000);
    putVolumeHeader(128, 28); // DXGI_FORMAT_R8G8B8A8_UNORM
    putVolumeHeader(132, 4);  // D3D10_RESOURCE_DIMENSION_TEXTURE3D
    putVolumeHeader(140, 1);
    {
        std::ofstream output(volumePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(volumeHeader.data()),
                     static_cast<std::streamsize>(volumeHeader.size()));
        const std::array<std::uint8_t, 20> payload{255, 0,   0,   255, 0, 255, 0,   255, 0,   0,
                                                   255, 255, 255, 255, 0, 255, 255, 255, 255, 255};
        output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    dayo::core::fx::MaterialGpuTableData table;
    table.textureSlotCount = 1;
    table.materialIndices = {0U, 1U};
    table.textureIndices2D = {0U, dayo::core::fx::kMissingMaterialTextureIndex};
    table.textureIndices3D = {0U, dayo::core::fx::kMissingMaterialTextureIndex};
    table.textures2D.push_back({.path = imagePath.string(),
                                .externalId = {},
                                .format = {},
                                .colorspace = {},
                                .mipPolicy = {},
                                .dimension = dayo::core::fx::MaterialTextureDimension::twoD,
                                .mipmapped = true});
    table.textures3D.push_back({.path = volumePath.string(),
                                .externalId = {},
                                .format = {},
                                .colorspace = {},
                                .mipPolicy = {},
                                .dimension = dayo::core::fx::MaterialTextureDimension::threeD,
                                .mipmapped = true});
    table.values.layout.stride = 16;
    table.values.count = 2;
    table.values.bytes.resize(32, std::byte{0x2A});

    MockDevice device;
    device.frameSlot_ = dayo::graphics::kNativeFramesInFlight - 1U;
    bool ok = false;
    {
        dayo::graphics::FxMaterialGpuRuntime runtime;
        std::string error;
        ok = check(runtime.sync(device, table, &error),
                   "MatDesc GPU runtime creates texture catalogs and table buffers");
        if (!ok)
            std::cerr << "MatDesc GPU runtime error: " << error << '\n';
        ok &= check(runtime.ready(), "MatDesc GPU runtime reports ready only after all resources exist");
        const auto bindings = runtime.bindings();
        ok &= check(bindings.materialIndices.valid() && bindings.textureIndices2D.valid() &&
                        bindings.textureIndices3D.valid() && bindings.values.valid(),
                    "MatDesc GPU runtime exposes the active frame-slot table buffers");
        ok &= check(bindings.textures2D.size() == 2 && bindings.textures3D.size() == 2 &&
                        bindings.textures2D.front().valid() && bindings.textures2D.back().valid() &&
                        bindings.textures3D.front().valid() && bindings.textures3D.back().valid(),
                    "MatDesc GPU runtime reserves descriptor index zero for dimension-correct fallback textures");
        ok &= check(device.textureDescs_.size() == 4 &&
                        device.textureDescs_[0].dimension == dayo::graphics::TextureDimension::d2 &&
                        device.textureDescs_[1].dimension == dayo::graphics::TextureDimension::d3 &&
                        device.textureDescs_[2].dimension == dayo::graphics::TextureDimension::d2 &&
                        device.textureDescs_[3].dimension == dayo::graphics::TextureDimension::d3 &&
                        device.textureDescs_[3].extent.depth == 2 && device.textureDescs_[3].mipLevels == 2 &&
                        (dayo::graphics::toBits(device.textureDescs_[2].usage) &
                         dayo::graphics::toBits(dayo::graphics::ResourceUsage::transferSrc)) != 0U,
                    "MatDesc GPU runtime allocates dimension-matched fallback, 2D, and DDS volume textures");
        const auto frameUploadCount = 4U * dayo::graphics::kNativeFramesInFlight;
        ok &=
            check(device.bufferDescs_.size() == frameUploadCount && device.bufferUploads_.size() == frameUploadCount &&
                      device.bufferUploads_[0].bytes.size() == 8 && device.bufferUploads_[1].bytes.size() == 8 &&
                      device.bufferUploads_[2].bytes.size() == 8 && device.bufferUploads_[3].bytes.size() == 32,
                  "new MatDesc buffers upload every frame slot before becoming ready");
        ok &= check(runtime.bindings().materialIndices == device.bufferUploads_[device.frameSlot_ * 4U].handle,
                    "MatDesc bindings select the initialized current frame slot");
        std::uint32_t uploadedTextureIndex = 0;
        std::uint32_t uploadedMissingIndex = 0;
        std::memcpy(&uploadedTextureIndex, device.bufferUploads_[1].bytes.data(), sizeof(uploadedTextureIndex));
        std::memcpy(&uploadedMissingIndex,
                    device.bufferUploads_[1].bytes.data() + static_cast<std::ptrdiff_t>(sizeof(uploadedMissingIndex)),
                    sizeof(uploadedMissingIndex));
        ok &= check(uploadedTextureIndex == 1U && uploadedMissingIndex == dayo::core::fx::kMissingMaterialTextureIndex,
                    "MatDesc GPU indices account for the reserved fallback descriptor at slot zero");
        std::uint32_t uploadedVolumeIndex = 0;
        std::memcpy(&uploadedVolumeIndex, device.bufferUploads_[2].bytes.data(), sizeof(uploadedVolumeIndex));
        ok &= check(uploadedVolumeIndex == 1U && device.textureUploads_ == 5 && device.generatedMipmaps_ == 1,
                    "MatDesc GPU runtime uploads 2D and 3D DDS mip chains with fallback-adjusted indices");

        const auto texturesBeforeResync = device.textureDescs_.size();
        const auto buffersBeforeResync = device.bufferDescs_.size();
        const auto uploadsBeforeResync = device.bufferUploads_.size();
        device.frameSlot_ = 0;
        ok &= check(runtime.sync(device, table, &error) && device.textureDescs_.size() == texturesBeforeResync &&
                        device.bufferDescs_.size() == buffersBeforeResync &&
                        device.bufferUploads_.size() == uploadsBeforeResync + 4U,
                    "unchanged MatDesc buffers upload only the active frame slot");

        dayo::fx::FxProgram materialProgram;
        materialProgram.materialDescriptor =
            dayo::core::EffectMaterialDescriptor{.name = "Surface", .templatePath = {}, .defaultFile = {}};
        dayo::core::EffectTexture localTexture;
        localTexture.name = "Lookup";
        localTexture.view = "SRV";
        localTexture.size.absolute = true;
        localTexture.size.width = 1;
        localTexture.size.height = 1;
        materialProgram.textures.push_back(std::move(localTexture));
        dayo::fx::FxDispatch materialPass;
        materialPass.name = "material-compute";
        materialPass.kind = dayo::fx::FxOpKind::compute;
        materialPass.executable = dayo::fx::FxComputeDispatch{"main"};
        materialProgram.passes.push_back(materialPass);

        MockDevice missingMaterialDevice;
        dayo::graphics::FxResourceRuntime missingMaterialRuntime;
        std::string missingMaterialError;
        ok &= check(!missingMaterialRuntime.initialize(missingMaterialDevice, materialProgram, testContext(),
                                                       &missingMaterialError, 2) &&
                        missingMaterialError.find("requires a ready GPU material runtime") != std::string::npos,
                    "MatDesc descriptor planning rejects a missing GPU table owner");

        dayo::graphics::FxResourceRuntime passRuntime;
        ok &= check(passRuntime.initialize(device, materialProgram, testContext(), &error, 2, &runtime),
                    "MatDesc descriptors join effect-local resources in the pass descriptor runtime");
        const auto materialLayouts = passRuntime.descriptorLayoutsFor(materialPass);
        ok &= check(materialLayouts.size() == 2 && materialLayouts[0].first == 2 && materialLayouts[1].first == 3,
                    "MatDesc pass descriptor plans produce main and 3D texture descriptor sets at planned set indices");
        const auto mat2dArray = std::ranges::find_if(device.descriptorLayoutsCreated_, [](const auto& layout) {
            return std::ranges::any_of(layout.bindings, [](const auto& binding) { return binding.count == 2; });
        });
        ok &= check(mat2dArray != device.descriptorLayoutsCreated_.end(),
                    "MatDesc descriptor array layouts use the physical catalog count including fallback textures");
        ok &= check(materialLayouts.size() == 2 &&
                        std::ranges::any_of(device.descriptorLayoutsCreated_[1].bindings,
                                            [](const auto& binding) { return binding.count == 2; }) &&
                        std::ranges::any_of(device.descriptorLayoutsCreated_[2].bindings,
                                            [](const auto& binding) { return binding.count == 2; }),
                    "MatDesc 2D and 3D descriptor arrays retain their independent catalog lengths");
        const auto slotZeroSets = passRuntime.descriptorSetsFor(materialPass);
        device.frameSlot_ = 1;
        const auto slotOneSets = passRuntime.descriptorSetsFor(materialPass);
        const auto setAt = [](const auto& sets, std::uint32_t setIndex) {
            return std::ranges::find_if(sets, [setIndex](const auto& set) { return set.setIndex == setIndex; });
        };
        const auto setZeroSlot0 = setAt(slotZeroSets, 2);
        const auto setZeroSlot1 = setAt(slotOneSets, 2);
        ok &= check(setZeroSlot0 != slotZeroSets.end() && setZeroSlot1 != slotOneSets.end() &&
                        setZeroSlot0->set != setZeroSlot1->set &&
                        runtime.bindings(0).materialIndices != runtime.bindings(1).materialIndices,
                    "MatDesc descriptor sets select the matching frame-safe material table buffers");
        device.frameSlot_ = 0;
        auto expandedTable = table;
        expandedTable.materialIndices = {0U, 1U, 2U};
        expandedTable.textureIndices2D = {0U, dayo::core::fx::kMissingMaterialTextureIndex,
                                          dayo::core::fx::kMissingMaterialTextureIndex};
        expandedTable.textureIndices3D = {0U, dayo::core::fx::kMissingMaterialTextureIndex,
                                          dayo::core::fx::kMissingMaterialTextureIndex};
        expandedTable.values.count = 3;
        expandedTable.values.bytes.resize(48, std::byte{0x31});
        std::array<dayo::graphics::handles::BufferHandle, dayo::graphics::kNativeFramesInFlight> oldSlotHandles{};
        for (std::size_t slot = 0; slot < dayo::graphics::kNativeFramesInFlight; ++slot)
            oldSlotHandles[slot] = runtime.bindings(slot).materialIndices;
        const auto buffersBeforeGrowth = device.bufferDescs_.size();
        const auto uploadsBeforeGrowth = device.bufferUploads_.size();
        const bool expanded = runtime.sync(device, expandedTable, &error);
        const auto refreshedSets = passRuntime.descriptorSetsFor(materialPass);
        ok &= check(expanded && device.bufferDescs_.size() == buffersBeforeGrowth + frameUploadCount &&
                        device.bufferUploads_.size() == uploadsBeforeGrowth + frameUploadCount,
                    "MatDesc growth reallocates and initializes every frame slot");
        for (std::size_t slot = 0; slot < dayo::graphics::kNativeFramesInFlight; ++slot)
            ok &= check(runtime.bindings(slot).materialIndices != oldSlotHandles[slot] &&
                            device.bufferUploads_[uploadsBeforeGrowth + slot * 4U].handle != oldSlotHandles[slot],
                        "MatDesc growth replaces every frame slot with its initialized allocation");
        ok &= check(runtime.bindings(0).materialIndices == device.bufferUploads_[uploadsBeforeGrowth].handle &&
                        !device.updatedDescriptorSets_.empty() && refreshedSets.size() == 2,
                    "MatDesc descriptors refresh to the initialized new table generation");
        passRuntime.reset();
    }
    ok &= check(device.destroyedTextures_ == 5 &&
                    device.destroyedBuffers_ == 2U * 4U * dayo::graphics::kNativeFramesInFlight,
                "MatDesc GPU runtime releases every resource generation and frame-slot buffer");

    auto invalidTable = table;
    invalidTable.textureIndices2D[0] = 1U;
    MockDevice invalidDevice;
    dayo::graphics::FxMaterialGpuRuntime invalidRuntime;
    std::string invalidError;
    ok &= check(!invalidRuntime.sync(invalidDevice, invalidTable, &invalidError) && !invalidError.empty() &&
                    invalidDevice.textureDescs_.empty() && invalidDevice.bufferDescs_.empty(),
                "MatDesc GPU runtime rejects out-of-range texture indices before allocating resources");
    fs::remove_all(directory, filesystemError);
    return ok;
}

bool testFxMaterialSceneRuntimeEvaluatesProjectAnnotations() {
    namespace fs = std::filesystem;
    const auto directory =
        fs::temp_directory_path() /
        ("dayo-matdesc-scene-runtime-" +
         std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    const auto projectDirectory = directory / "project";
    const auto annotationDirectory = projectDirectory / "materials";
    std::error_code filesystemError;
    fs::create_directories(annotationDirectory, filesystemError);
    if (filesystemError)
        return check(false, "MatDesc scene runtime test directory created");

    const auto annotationPath = annotationDirectory / "surface.txt";
    const std::string initialAnnotation = "Weight : frac(Time)\n_TAlbedo : albedo.ppm\n_TScreen : screen.bmp\n"
                                          "_TDeformed : DeformerOutput\n";
    {
        std::ofstream output(annotationPath, std::ios::binary);
        output << initialAnnotation;
    }
    const auto imagePath = annotationDirectory / "albedo.ppm";
    {
        std::ofstream output(imagePath, std::ios::binary);
        output << "P6\n1 1\n255\n";
        output.put(static_cast<char>(64));
        output.put(static_cast<char>(128));
        output.put(static_cast<char>(255));
    }

    auto schema = dayo::core::fx::parseMaterialTemplateSchema(
        "f.1 : Weight\n_T0 : Albedo\n_T1 : Screen\n_T2 : Deformed\nWeight : 0.75\n", "Surface");
    dayo::core::MaterialEditorState material;
    material.annotation = "materials/surface.txt";
    const std::array materials{material};
    const std::array models{dayo::graphics::FxMaterialSceneModel{
        .id = 17,
        .sourcePath = directory / "models" / "avatar.pmx",
        .projectDirectory = projectDirectory,
        .modelIndex = 0,
        .vertexCount = 12,
        .cloneCount = 2,
        .materials = materials,
    }};

    MockDevice device;
    dayo::graphics::FxMaterialSceneRuntime runtime;
    dayo::fx::FxFrameContext context;
    context.renderWidth = 64;
    context.renderHeight = 32;
    context.totalMaterial = 1;
    context.frame = 3.0F;
    context.time = 10.25;
    const dayo::graphics::FxMaterialTextureResolver resolver =
        [](dayo::core::ModelId, const dayo::core::fx::MaterialTextureSchema&,
           std::string_view assigned) -> std::optional<dayo::graphics::FxMaterialExternalTexture> {
        if (assigned == "screen.bmp")
            return dayo::graphics::FxMaterialExternalTexture{
                .identity = "host:ScreenBMP:mock",
                .texture = {700, 1},
                .dimension = dayo::core::fx::MaterialTextureDimension::twoD,
                .generation = 1,
            };
        if (assigned == "DeformerOutput")
            return dayo::graphics::FxMaterialExternalTexture{
                .identity = "deformer:17:4:1:DeformerOutput",
                .texture = {701, 2},
                .dimension = dayo::core::fx::MaterialTextureDimension::twoD,
                .generation = 4,
            };
        return std::nullopt;
    };
    std::string error;
    bool ok = check(runtime.sync(device, schema, models, context, &error, resolver),
                    "MatDesc scene runtime links project-relative material annotation and GPU resources");
    if (!ok)
        std::cerr << "MatDesc scene runtime error: " << error << '\n';

    const auto readUploadedWeight = [&device]() {
        float value = 0.0F;
        if (device.bufferUploads_.empty() || device.bufferUploads_.back().bytes.size() < sizeof(value))
            return value;
        std::memcpy(&value, device.bufferUploads_.back().bytes.data(), sizeof(value));
        return value;
    };
    const auto textures = runtime.gpuRuntime().bindings().textures2D;
    ok &= check(
        runtime.gpuRuntime().ready() && textures.size() == 4 &&
            std::ranges::find(textures, dayo::graphics::handles::TextureHandle{700, 1}) != textures.end() &&
            std::ranges::find(textures, dayo::graphics::handles::TextureHandle{701, 2}) != textures.end() &&
            device.textureDescs_.size() == 3 && std::abs(readUploadedWeight() - 0.25F) < 1.0e-6F,
        "MatDesc scene runtime evaluates file expressions and binds reserved/deformer textures without owning them");
    ok &= check(!runtime.descriptorLayoutChanged(),
                "first MatDesc scene synchronization does not report a prior descriptor layout change");

    context.time = 2.5;
    ok &= check(runtime.sync(device, schema, models, context, &error, resolver) &&
                    std::abs(readUploadedWeight() - 0.5F) < 1.0e-6F && !runtime.descriptorLayoutChanged() &&
                    device.textureDescs_.size() == 3,
                "MatDesc scene runtime reevaluates expressions and reuses file/external texture catalogs");

    const auto originalTimestamp = fs::last_write_time(annotationPath, filesystemError);
    const std::string unchangedSuffix = "\n_TAlbedo : albedo.ppm\n_TScreen : screen.bmp\n"
                                        "_TDeformed : DeformerOutput\n";
    std::string sameSizeAnnotation = "Weight : 0.25";
    sameSizeAnnotation.append(initialAnnotation.size() - unchangedSuffix.size() - sameSizeAnnotation.size(), ' ');
    sameSizeAnnotation += unchangedSuffix;
    {
        std::ofstream output(annotationPath, std::ios::binary | std::ios::trunc);
        output << sameSizeAnnotation;
    }
    fs::last_write_time(annotationPath, originalTimestamp, filesystemError);
    ok &= check(!filesystemError && sameSizeAnnotation.size() == initialAnnotation.size(),
                "MatDesc annotation fixture changes content while preserving size and timestamp");
    std::this_thread::sleep_for(std::chrono::milliseconds{550});
    ok &=
        check(runtime.sync(device, schema, models, context, &error) && std::abs(readUploadedWeight() - 0.25F) < 1.0e-6F,
              "MatDesc annotation polling detects same-size edits with unchanged filesystem timestamps");

    {
        std::ofstream output(annotationPath, std::ios::binary | std::ios::trunc);
        output << "Weight : frac(Time)\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{550});
    ok &= check(runtime.sync(device, schema, models, context, &error) && runtime.descriptorLayoutChanged() &&
                    runtime.gpuRuntime().bindings().textures2D.size() == 1,
                "MatDesc scene runtime reports descriptor-array shape changes after annotation relinking");

    fs::remove_all(directory, filesystemError);
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
    catalog.add({"Preview", dayo::fx::FxCatalogGroup::rendererDirectory, dayo::core::fx::FxCategory::render,
                 "renderer/Preview.fxdayo", true, 0});
    catalog.add({"Blur", dayo::fx::FxCatalogGroup::postprocessDirectory, dayo::core::fx::FxCategory::postprocess,
                 "postprocess/Blur.fxdayo", true, 10});
    catalog.add({"tonemap", dayo::fx::FxCatalogGroup::postprocessDirectory, dayo::core::fx::FxCategory::postprocess,
                 "postprocess/tonemap.fxdayo", true, 50});
    catalog.add({"Grain", dayo::fx::FxCatalogGroup::postprocessDirectory, dayo::core::fx::FxCategory::postprocess,
                 "postprocess/Grain.fxdayo", true, 200});
    catalog.add({"Smoke", dayo::fx::FxCatalogGroup::particleDirectory, dayo::core::fx::FxCategory::render,
                 "particle/Smoke.fxdayo", true, 5});
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

    dayo::core::Scene scene;
    dayo::core::EffectGraph deform;
    deform.sourcePath = "deform/Cloth.fxdayo";
    deform.category = "deform";
    dayo::core::EffectGraph renderer;
    renderer.sourcePath = "renderer/Preview.fxdayo";
    renderer.category = "render";
    dayo::core::EffectGraph post;
    post.sourcePath = "postprocess/ACESTonemap.fxdayo";
    post.category = "postprocess";
    const auto deformId = scene.addEffect(std::move(deform), 7, 2);
    static_cast<void>(scene.addEffect(std::move(renderer), 7, 0));
    static_cast<void>(scene.addEffect(std::move(post), 8, 5));
    const auto stacked = scheduler.schedule(
        scene.effects(), [](dayo::core::ModelId id) -> std::optional<dayo::core::ModelExecutionOrder> {
            if (id == 7)
                return dayo::core::ModelExecutionOrder{.deform = 1};
            if (id == 8)
                return dayo::core::ModelExecutionOrder{.postprocess = 10};
            return std::nullopt;
        });
    ok &= check(stacked.size() == 5 && stacked[1].name == "Cloth" && stacked[1].effectId == deformId &&
                    stacked[2].name == "Preview" && stacked[2].effectId != 0 && stacked[3].name == "ACESTonemap" &&
                    stacked[3].effectId != 0 && stacked[3].stage == dayo::fx::FrameStage::postPre &&
                    stacked[1].order == 3 && stacked[3].order == 15,
                "effect stack scheduler follows canonical category and model order");
    ok &= check(scene.effects().renderer.has_value() && scene.effects().deform.size() == 1 &&
                    scene.effects().postprocess.size() == 1 && scene.effect() != nullptr,
                "scene stores deform renderer and postprocess instances separately");
    ok &= check(scene.removeEffect(deformId) && scene.effects().deform.empty(), "scene removes an effect by id");
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

    dayo::core::EffectGraph graphicsGraph;
    graphicsGraph.sourcePath = "graphics-state.fxdayo";
    graphicsGraph.category = "render";
    dayo::core::EffectPass graphicsPass;
    graphicsPass.name = "MRT";
    graphicsPass.type = dayo::core::EffectPassType::rasterizer;
    graphicsPass.vertexShader = "VS";
    graphicsPass.pixelShader = "PS";
    graphicsPass.graphics.rasterizer.cullMode = dayo::core::EffectCullMode::front;
    graphicsPass.renderTargets = {{.name = "Color0", .clear = true, .clearValue = {}},
                                  {.name = "Color1", .clear = false, .clearValue = {}},
                                  {.name = "Color2", .clear = false, .clearValue = {}}};
    graphicsPass.depth = {.name = "Depth", .clear = true, .clearValue = {}};
    graphicsGraph.passes.push_back(graphicsPass);
    const auto graphicsProgram = dayo::fx::FxCompiler{}.compile(graphicsGraph);
    ok &= check(graphicsProgram.passes.size() == 1 && graphicsProgram.passes.front().resources.size() == 4,
                "compiler keeps all graphics attachments");
    if (!graphicsProgram.passes.empty()) {
        const auto& dispatch = graphicsProgram.passes.front();
        const auto* raster = std::get_if<dayo::fx::FxRasterDispatch>(&dispatch.executable);
        ok &= check(raster != nullptr && raster->colorAttachments.size() == 3 && raster->depthAttachment.has_value() &&
                        raster->graphics.rasterizer.cullMode == dayo::core::EffectCullMode::front,
                    "raster dispatch keeps graphics state");
        if (dispatch.resources.size() == 4)
            ok &= check(dispatch.resources[0].role == dayo::fx::FxResourceRole::colorAttachment &&
                            dispatch.resources[2].role == dayo::fx::FxResourceRole::colorAttachment &&
                            dispatch.resources[3].role == dayo::fx::FxResourceRole::depthAttachment,
                        "resource roles distinguish MRT and DSV");
    }

    const auto oidnSource = R"FX([YRZFX]
{
  fx: {
    category: "postprocess",
    passes: [{name: "Denoise", type: "oidn", inputs: ["Beauty", "Albedo", "Normal"], RTV: ["Denoised"]}],
  },
}
[HLSL]
)FX";
    const auto oidnProgram =
        dayo::fx::FxCompiler{}.compileSource(dayo::fx::makeFxSourceDocument("oidn.fxdayo", oidnSource));
    ok &= check(oidnProgram.passes.size() == 1 && oidnProgram.passes.front().kind == dayo::fx::FxOpKind::oidn &&
                    oidnProgram.passes.front().resources.size() == 4,
                "compiler preserves parsed OIDN input and output resources");
    if (!oidnProgram.passes.empty()) {
        const auto* oidn = std::get_if<dayo::fx::FxOidnDispatch>(&oidnProgram.passes.front().executable);
        ok &= check(oidn != nullptr && oidn->input == "Beauty" && oidn->albedo == "Albedo" &&
                        oidn->normal == "Normal" && oidn->output == "Denoised",
                    "compiler builds a typed OIDN dispatch");
    }
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
    pass.unorderedAccess.push_back({"Color", true, {}});
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
    bool ok =
        check(runtime.initialize(device, program, context, &error), "FX resource runtime materializes declarations");
    ok &= check(error.empty(), "FX resource runtime has no initialization error");
    ok &= check(runtime.ready() && runtime.resourceCount() == 4, "FX resource runtime owns every declaration");
    const auto colorTexture = runtime.resolveTexture("Color");
    const auto volumeTexture = runtime.resolveTexture("Volume");
    const auto lightBuffer = runtime.resolveBuffer("Lights");
    const auto linearSampler = runtime.resolveSampler("Linear");
    ok &=
        check(colorTexture.has_value() && colorTexture->valid() && volumeTexture.has_value() && volumeTexture->valid(),
              "FX resource runtime resolves 2D and 3D textures");
    ok &= check(lightBuffer.has_value() && lightBuffer->valid() && linearSampler.has_value() && linearSampler->valid(),
                "FX resource runtime resolves buffers and samplers");
    const auto colorExtent = runtime.extent("Color");
    const auto volumeExtent = runtime.extent("Volume");
    const auto colorSymbolExtent = runtime.find("Color");
    const auto volumeSymbolExtent = runtime.find("Volume");
    const auto bufferSymbolExtent = runtime.find("Lights");
    ok &= check(colorExtent.has_value() && colorExtent->width == 64 && colorExtent->height == 32,
                "FX resource runtime resolves absolute 2D extents");
    ok &= check(volumeExtent.has_value() && volumeExtent->width == 8 && volumeExtent->height == 4 &&
                    volumeExtent->depth == 2,
                "FX resource runtime resolves absolute 3D extents");
    ok &= check(colorSymbolExtent.has_value() && colorSymbolExtent->dimension == 2 && volumeSymbolExtent.has_value() &&
                    volumeSymbolExtent->dimension == 3 && bufferSymbolExtent.has_value() &&
                    bufferSymbolExtent->dimension == 1,
                "FX resource runtime exposes dimension-preserving expression symbols");
    ok &= check(runtime.descriptorLayout().valid() && runtime.descriptorSet().valid() &&
                    runtime.descriptorLayoutDesc().bindings.size() == 4 && device.descriptorBindings_.size() == 4,
                "FX resource runtime allocates one typed descriptor set");
    ok &= check(device.descriptorLayout_.bindings[0].kind == dayo::graphics::DescriptorKind::sampledImage &&
                    device.descriptorLayout_.bindings[1].kind == dayo::graphics::DescriptorKind::storageImage &&
                    device.descriptorLayout_.bindings[2].kind == dayo::graphics::DescriptorKind::storageBuffer &&
                    device.descriptorLayout_.bindings[3].kind == dayo::graphics::DescriptorKind::sampler,
                "FX resource runtime derives descriptor kinds from views");
    ok &= check(
        device.descriptorLayout_.bindings[0].binding == 16 && device.descriptorLayout_.bindings[1].binding == 0 &&
            device.descriptorLayout_.bindings[2].binding == 1 && device.descriptorLayout_.bindings[3].binding == 32,
        "FX resource runtime aligns descriptor slots with HLSL register classes");
    dayo::fx::FxDispatch dispatch;
    ok &= check(runtime.resolveDescriptorSet(dispatch).has_value(), "FX resource runtime resolves pass descriptor set");
    runtime.reset();
    ok &= check(device.destroyedTextures_ == 2 && device.destroyedBuffers_ == 1 && device.destroyedSamplers_ == 1 &&
                    device.destroyedDescriptorLayouts_ == 1 && device.destroyedDescriptorSets_ == 1,
                "FX resource runtime releases all typed allocations");
    return ok;
}

bool testDepthTextureUsageIncludesSampling() {
    dayo::fx::FxProgram program;
    dayo::core::EffectTexture depth;
    depth.name = "Depth";
    depth.format = "D32_FLOAT";
    depth.view = "DSV";
    depth.size.absolute = true;
    depth.size.width = 64;
    depth.size.height = 32;
    program.textures.push_back(std::move(depth));

    dayo::fx::FxDispatch depthWrite;
    depthWrite.name = "depth-write";
    depthWrite.resources.push_back({"Depth", true, dayo::fx::FxResourceRole::depthAttachment});
    dayo::fx::FxDispatch depthSample;
    depthSample.name = "depth-sample";
    depthSample.resources.push_back({"Depth", false, dayo::fx::FxResourceRole::sampled});
    program.passes = {depthWrite, depthSample};

    MockDevice device;
    dayo::graphics::FxResourceRuntime runtime;
    std::string error;
    if (!runtime.initialize(device, program, testContext(), &error))
        return check(false, "depth attachment plus sampled usage resource allocation succeeds");
    const auto usage = dayo::graphics::toBits(device.textureDescs_.front().usage);
    const auto required = dayo::graphics::toBits(dayo::graphics::ResourceUsage::depthRead) |
                          dayo::graphics::toBits(dayo::graphics::ResourceUsage::depthWrite) |
                          dayo::graphics::toBits(dayo::graphics::ResourceUsage::sampledRead);
    const bool valid = (usage & required) == required;
    runtime.reset();
    return check(valid, "depth texture usage is unioned across attachment writes and sampled reads");
}

bool testFxExternalTextureMetadataAndUpload() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "dayo-fx-external-texture-test";
    std::error_code error;
    fs::create_directories(directory, error);
    if (error)
        return check(false, "external FX texture test directory created");
    const auto imagePath = directory / "source.ppm";
    {
        std::ofstream output(imagePath, std::ios::binary);
        output << "P6\n2 1\n255\n";
        output.put(static_cast<char>(255));
        output.put(static_cast<char>(0));
        output.put(static_cast<char>(0));
        output.put(static_cast<char>(0));
        output.put(static_cast<char>(255));
        output.put(static_cast<char>(0));
    }

    dayo::fx::FxProgram program;
    program.sourcePath = directory / "effect.fxdayo";
    dayo::core::EffectTexture texture;
    texture.name = "Input";
    texture.filename = imagePath.filename().string();
    texture.mipmap = true;
    texture.view = "SRV";
    program.textures.push_back(std::move(texture));
    MockDevice device;
    dayo::graphics::FxResourceRuntime runtime;
    std::string runtimeError;
    bool ok = check(runtime.initialize(device, program, testContext(), &runtimeError),
                    "FX external texture initializes from a relative filename");
    ok &= check(runtimeError.empty() && runtime.extent("Input").has_value() && runtime.extent("Input")->width == 2 &&
                    runtime.extent("Input")->height == 1,
                "FX external texture uses decoded dimensions when size is omitted");
    ok &= check(!device.textureDescs_.empty() && device.textureDescs_.front().mipLevels == 2 &&
                    device.uploadedTextureBytes_ == 8 && device.generatedMipmaps_ == 1,
                "FX external texture uploads base mip and generates remaining mips");
    runtime.reset();

    const auto volumePath = directory / "volume.dds";
    std::array<std::uint8_t, 148> volumeHeader{};
    const auto putVolumeHeader = [&volumeHeader](std::size_t offset, std::uint32_t value) {
        volumeHeader[offset] = static_cast<std::uint8_t>(value);
        volumeHeader[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
        volumeHeader[offset + 2] = static_cast<std::uint8_t>(value >> 16U);
        volumeHeader[offset + 3] = static_cast<std::uint8_t>(value >> 24U);
    };
    std::copy_n("DDS ", 4, volumeHeader.begin());
    putVolumeHeader(4, 124);
    putVolumeHeader(12, 1);
    putVolumeHeader(16, 2);
    putVolumeHeader(24, 2);
    putVolumeHeader(28, 2);
    putVolumeHeader(76, 32);
    putVolumeHeader(80, 4);
    putVolumeHeader(84, 0x30315844); // DX10
    putVolumeHeader(108, 0x1000);
    putVolumeHeader(112, 0x200000);
    putVolumeHeader(128, 28); // DXGI_FORMAT_R8G8B8A8_UNORM
    putVolumeHeader(132, 4);  // D3D10_RESOURCE_DIMENSION_TEXTURE3D
    putVolumeHeader(140, 1);
    {
        std::ofstream output(volumePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(volumeHeader.data()),
                     static_cast<std::streamsize>(volumeHeader.size()));
        const std::array<std::uint8_t, 20> payload{255, 0,   0,   255, 0, 255, 0,   255, 0,   0,
                                                   255, 255, 255, 255, 0, 255, 255, 255, 255, 255};
        output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    dayo::fx::FxProgram volumeProgram;
    volumeProgram.sourcePath = directory / "volume.fxdayo";
    dayo::core::EffectTexture volumeTexture;
    volumeTexture.name = "SmokeVolume";
    volumeTexture.filename = volumePath.filename().string();
    volumeTexture.mipmap = true;
    volumeTexture.view = "SRV";
    volumeProgram.textures3D.push_back(std::move(volumeTexture));
    const auto uploadsBeforeVolume = device.textureUploads_;
    dayo::graphics::FxResourceRuntime volumeRuntime;
    runtimeError.clear();
    ok &= check(volumeRuntime.initialize(device, volumeProgram, testContext(), &runtimeError),
                "FX external Texture3D loads a DX10 DDS volume");
    ok &= check(runtimeError.empty() && volumeRuntime.extent("SmokeVolume").has_value() &&
                    volumeRuntime.extent("SmokeVolume")->width == 2 &&
                    volumeRuntime.extent("SmokeVolume")->height == 1 && volumeRuntime.extent("SmokeVolume")->depth == 2,
                "external FX volume uses the DDS extent when no explicit size is declared");
    ok &= check(device.textureDescs_.back().dimension == dayo::graphics::TextureDimension::d3 &&
                    device.textureDescs_.back().mipLevels == 2 && device.textureDescs_.back().extent.depth == 2 &&
                    device.textureUploads_ == uploadsBeforeVolume + 2 &&
                    device.uploadedTextureData_ == std::vector<std::uint8_t>{255, 255, 255, 255},
                "FX runtime allocates a 3D texture and uploads its DDS mip chain");
    volumeRuntime.reset();
    fs::remove_all(directory, error);
    return ok;
}

bool testNativeFxRuntimeRefreshesFrameResources() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;

    dayo::fx::FxProgram program;
    program.sourcePath = "native-refresh.fxdayo";
    program.hlsl = "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {}\n";
    dayo::core::EffectTexture output;
    output.name = "Output";
    output.view = "UAV";
    program.textures.push_back(std::move(output));
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "refresh-pass";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.resources.push_back({"Output", true});
    dispatch.conditions.emplace_back("frame if FRAME >= 12");
    program.passes.push_back(dispatch);
    auto startDispatch = dispatch;
    startDispatch.name = "start-pass";
    startDispatch.conditions = {"start"};
    program.passes.push_back(std::move(startDispatch));
    auto resizeDispatch = dispatch;
    resizeDispatch.name = "resize-pass";
    resizeDispatch.conditions = {"resize"};
    program.passes.push_back(std::move(resizeDispatch));
    auto loadDispatch = dispatch;
    loadDispatch.name = "load-pass";
    loadDispatch.conditions = {"load"};
    program.passes.push_back(std::move(loadDispatch));

    const auto firstContext = dayo::fx::makeFxFrameContext(12.0F, 3, 4, 2, 1, 0, 3, 1, 1, 1);
    const auto secondContext = dayo::fx::makeFxFrameContext(12.0F, 3, 8, 4, 1, 0, 3, 1, 1, 1);
    const std::array sharedLayouts{dayo::graphics::handles::DescriptorSetLayoutHandle{700, 1}};
    const std::array sharedSets{dayo::graphics::handles::DescriptorSetHandle{800, 1}};
    MockDevice device;
    dayo::graphics::DayoFxRuntime runtime;
    std::string error;
    bool ok = check(runtime.initializeForFrame(device, std::move(program), compiler, firstContext, sharedLayouts,
                                               &error, sharedSets),
                    "native FX runtime initializes against the first frame context");
    const auto firstExtent = runtime.nativeRuntime().resources().extent("Output");
    ok &= check(firstExtent.has_value() && firstExtent->width == 4 && firstExtent->height == 2,
                "native FX runtime uses the first frame dimensions");
    ok &= check(runtime.refresh(firstContext, &error), "native FX runtime reuses unchanged frame resources");
    const auto allocationsBeforeRefresh = device.textureDescs_.size();
    ok &= check(runtime.refresh(secondContext, &error), "native FX runtime refreshes changed frame resources");
    const auto secondExtent = runtime.nativeRuntime().resources().extent("Output");
    ok &= check(secondExtent.has_value() && secondExtent->width == 8 && secondExtent->height == 4,
                "native FX runtime rebuilds render-size-dependent resources");
    ok &= check(device.textureDescs_.size() == allocationsBeforeRefresh + 1,
                "native FX runtime does not rebuild resources for an unchanged context");

    auto temporalContext = secondContext;
    temporalContext.frame += 1.0F;
    temporalContext.sample += 1U;
    temporalContext.camera.distance += 0.25F;
    temporalContext.lighting.color[0] += 0.1F;
    const auto allocationsBeforeTemporalRefresh = device.textureDescs_.size();
    ok &= check(runtime.refresh(temporalContext, &error),
                "native FX runtime accepts a new frame without rebuilding resources");
    ok &= check(device.textureDescs_.size() == allocationsBeforeTemporalRefresh,
                "native FX runtime keeps persistent resources across frame changes");

    dayo::graphics::FxExecutionResources resources;
    resources.resolveDescriptorSets = [](const dayo::fx::FxDispatch&) {
        return std::vector<dayo::graphics::FxExecutionResources::TypedDescriptorSetBinding>{{{900, 1}, 0}};
    };
    auto frame = runtime.prepareFrame(secondContext);
    MockCommands commands;
    frame.context.frame = 11.0F;
    const auto firstInvocation = runtime.execute(frame, commands, resources);
    ok &= check(firstInvocation.compute == 1,
                "Dayo FX runtime raises OnLoad on first use without synthesizing OnStart or OnResize");
    frame = runtime.prepareFrame(secondContext);
    frame.context.frame = 12.0F;
    const auto frameInvocation = runtime.execute(frame, commands, resources);
    ok &= check(frameInvocation.compute == 1,
                "Dayo FX runtime evaluates the frame predicate without repeating one-shot events");
    frame = runtime.prepareFrame(secondContext);
    frame.context.host.onStart = true;
    const auto startInvocation = runtime.execute(frame, commands, resources);
    ok &= check(startInvocation.compute == 2, "Dayo FX runtime honors the host playback-start event");
    frame = runtime.prepareFrame(secondContext);
    frame.context.host.onStart = false;
    frame.context.host.onResize = true;
    const auto resizeInvocation = runtime.execute(frame, commands, resources);
    ok &= check(resizeInvocation.compute == 2, "Dayo FX runtime honors the host resize event");
    const auto descriptorCount =
        static_cast<std::size_t>(std::count(commands.trace.begin(), commands.trace.end(), std::string{"descriptorEx"}));
    ok &= check(descriptorCount == 12, "native FX runtime binds the shared and effect resource sets per pass");
    runtime.reset();
    return ok;
}

bool testNativeFxRuntimeBindsResourcesAndPipelines() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;

    dayo::fx::FxProgram program;
    program.sourcePath = "native-runtime.fxdayo";
    program.hlsl = "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {}\n";
    dayo::core::EffectBuffer lights;
    lights.name = "Lights";
    lights.type = "float4";
    lights.view = "UAV";
    lights.elementSize = 16;
    lights.size.absolute = true;
    lights.size.width = 4;
    program.buffers.push_back(std::move(lights));
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "native-pass";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.resources.push_back({"Lights", true});
    program.passes.push_back(std::move(dispatch));

    MockDevice device;
    dayo::graphics::NativeFxRuntime runtime;
    std::string error;
    bool ok = check(runtime.initialize(device, std::move(program), compiler, {}, &error),
                    "native FX runtime initializes resources and pipelines");
    ok &= check(error.empty() && runtime.ready() && runtime.pipelineLayout().valid() && runtime.resources().ready() &&
                    runtime.pipelines().size() == 1,
                "native FX runtime owns the complete program lifetime");
    if (!ok)
        return false;

    auto frame = runtime.prepareFrame(testContext());
    MockCommands commands;
    const auto stats = runtime.execute(frame, commands);
    ok &= check(stats.compute == 1, "native FX runtime executes the planned compute pass");
    ok &=
        check(commands.trace == std::vector<std::string>{"descriptorEx", "bindEx", "dispatch:4x4x1", "memoryBarrierEx"},
              "native FX runtime binds its resource set before dispatch");
    runtime.reset();
    ok &= check(device.destroyedPipelines_ == 1 && device.destroyedShaders_ == 1 &&
                    device.destroyedDescriptorSets_ == 2 && device.destroyedDescriptorLayouts_ == 2 &&
                    device.destroyedPipelineLayouts_ == 1,
                "native FX runtime tears down pipeline and resource ownership");
    return ok;
}

bool testNativeFxRuntimeBindsFixedSceneSets() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;

    dayo::fx::FxProgram program;
    program.sourcePath = "native-scene-runtime.fxdayo";
    program.hlsl = "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {}\n";
    dayo::core::EffectTexture output;
    output.name = "Output";
    output.view = "UAV";
    program.textures.push_back(std::move(output));
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "native-scene-pass";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.resources.push_back({"Output", true});
    program.passes.push_back(std::move(dispatch));

    std::array<dayo::graphics::handles::DescriptorSetLayoutHandle, 10> layouts{};
    std::array<dayo::graphics::handles::DescriptorSetHandle, 10> sets{};
    for (std::size_t index = 0; index < layouts.size(); ++index) {
        layouts[index] = {static_cast<std::uint32_t>(index + 1), 1};
        sets[index] = {static_cast<std::uint32_t>(index + 101), 1};
    }
    MockDevice device;
    dayo::graphics::NativeFxRuntime runtime;
    std::string error;
    const auto context = testContext();
    bool ok = check(runtime.initializeForFrame(device, std::move(program), compiler, context, layouts, &error, sets),
                    "native FX runtime accepts fixed native scene layouts and sets");
    ok &= check(error.empty() && runtime.resourceSetIndex() == dayo::graphics::kNativeFxResourceSet &&
                    runtime.sharedDescriptorSetCount() == dayo::graphics::kNativeSceneDescriptorSetCount,
                "native FX runtime places FX resources after all native scene spaces");
    if (!ok)
        return false;
    auto frame = runtime.prepareFrame(context);
    MockCommands commands;
    const auto stats = runtime.execute(frame, commands);
    const auto descriptorCount =
        static_cast<std::size_t>(std::count(commands.trace.begin(), commands.trace.end(), std::string{"descriptorEx"}));
    ok &= check(stats.compute == 1 && descriptorCount == dayo::graphics::kNativeSceneDescriptorSetCount + 1,
                "native FX runtime binds every native scene set before FX resources");
    runtime.reset();
    return ok;
}

bool testNativeFxRuntimeExecutesMatDescDescriptorSets() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;

    dayo::fx::FxProgram program;
    program.sourcePath = "native-matdesc-runtime.fxdayo";
    program.materialDescriptor =
        dayo::core::EffectMaterialDescriptor{.name = "Surface", .templatePath = {}, .defaultFile = {}};
    program.materialSchema =
        dayo::core::fx::parseMaterialTemplateSchema("f.1 : Roughness\n_T0 : Albedo\n_V1 : Volume\n", "Surface");
    dayo::core::EffectTexture output;
    output.name = "Output";
    output.view = "UAV";
    output.size.absolute = true;
    output.size.width = 1;
    output.size.height = 1;
    program.textures.push_back(std::move(output));
    program.hlsl = "#ifdef YRZ_PASS_matdesc\n"
                   "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { Output[id.xy] = "
                   "float4(0, 0, 0, 1); }\n"
                   "#endif\n";
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "matdesc";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.numThreads = {1, 1, 1};
    dispatch.resources.push_back({"Output", true});
    program.passes.push_back(dispatch);

    dayo::core::fx::MaterialGpuTableData table;
    table.textureSlotCount = 2;
    table.materialIndices = {0U};
    table.textureIndices2D = {dayo::core::fx::kMissingMaterialTextureIndex,
                              dayo::core::fx::kMissingMaterialTextureIndex};
    table.textureIndices3D = {dayo::core::fx::kMissingMaterialTextureIndex,
                              dayo::core::fx::kMissingMaterialTextureIndex};
    table.values.layout.stride = sizeof(std::uint32_t);
    table.values.count = 1;
    table.values.bytes.resize(sizeof(std::uint32_t), std::byte{0});

    MockDevice device;
    dayo::graphics::FxMaterialGpuRuntime materialRuntime;
    dayo::graphics::NativeFxRuntime runtime;
    dayo::fx::FxNativeShaderSourceOptions sourceOptions;
    sourceOptions.preamble = "#define NonUniformResourceIndex(value) (value)\n";
    std::string error;
    bool ok = check(materialRuntime.sync(device, table, &error), "MatDesc native test prepares GPU table resources");
    ok &= check(runtime.initializeForFrame(device, std::move(program), compiler, testContext(), {}, &error, {},
                                           sourceOptions, &materialRuntime),
                "native FX pipeline initializes with generated MatDesc texture accessors");
    if (!ok) {
        if (!error.empty())
            std::cerr << "Native MatDesc runtime error: " << error << '\n';
        return false;
    }
    ok &= check(device.pipelineLayoutDesc_.setLayouts.size() == 2 &&
                    std::ranges::all_of(device.pipelineLayoutDesc_.setLayouts,
                                        [](const auto layout) { return layout.valid(); }),
                "native MatDesc pipeline layout includes contiguous main and 3D descriptor sets");
    auto frame = runtime.prepareFrame(testContext());
    MockCommands commands;
    const auto stats = runtime.execute(frame, commands);
    ok &= check(stats.compute == 1 && commands.descriptorSetIndices_ == std::vector<std::uint32_t>{0U, 1U},
                "native FX binds MatDesc main and secondary 3D sets at their shader set indices");
    runtime.reset();
    materialRuntime.reset();
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
    namespace fs = std::filesystem;
    const auto directory =
        fs::temp_directory_path() /
        ("dayo-fx-shader-include-" +
         std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code error;
    fs::create_directories(directory, error);
    if (error)
        return check(false, "shader compiler include directory created");
    {
        std::ofstream include(directory / "constants.hlsli");
        include << "#define TEST_SHADER_VALUE 1.0\n";
    }
    dayo::fx::FxShaderCompileRequest request;
    request.sourcePath = directory / "compiler-test.hlsl";
    request.entryPoint = "main";
    request.stage = dayo::fx::FxShaderStage::fragment;
    request.includeDirectories.push_back(directory);
    request.hlsl = "#include \"constants.hlsli\"\n"
                   "float4 main() : SV_Target { return float4(TEST_SHADER_VALUE, 0, 0, 1); }\n";
    const auto artifact = compiler.compile(request);
    bool ok = true;
    ok &=
        check(!artifact.spirv.empty() && artifact.spirv.front() == 0x07230203U, "real shader compiler returns SPIR-V");
    ok &= check(!artifact.compilerVersion.empty(), "real shader compiler records its version");

    dayo::fx::FxShaderCompileRequest resourceRequest;
    resourceRequest.sourcePath = directory / "resource-registers.hlsl";
    resourceRequest.entryPoint = "main";
    resourceRequest.stage = dayo::fx::FxShaderStage::compute;
    resourceRequest.hlsl = "RWTexture2D<float4> U : register(u0);\n"
                           "Texture2D<float4> T : register(t0);\n"
                           "SamplerState S : register(s0);\n"
                           "cbuffer B : register(b0) { float4 x; }\n"
                           "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) "
                           "{ U[id.xy] = T.SampleLevel(S, float2(0, 0), 0) + x; }\n";
    const auto resourceArtifact = compiler.compile(resourceRequest);
    ok &= check(hasUniqueDescriptorBindings(resourceArtifact.spirv),
                "glslc keeps HLSL register classes in distinct bindings");
    auto nativeFxRequest = resourceRequest;
    nativeFxRequest.requireDxcForNativeFxAbi = true;
    const bool dxc = compiler.executable().filename() == "dxc" || compiler.executable().filename() == "dxc.exe";
    if (dxc) {
        const auto nativeFxArtifact = compiler.compile(nativeFxRequest);
        ok &= check(!nativeFxArtifact.spirv.empty(), "DXC compiles native FX with the explicit globals ABI");
    } else {
        bool rejectedFallback = false;
        try {
            static_cast<void>(compiler.compile(nativeFxRequest));
        } catch (const std::runtime_error& exception) {
            rejectedFallback = std::string_view(exception.what()).find("require DXC") != std::string_view::npos;
        }
        ok &= check(rejectedFallback, "native FX rejects glslc when it cannot guarantee the globals ABI");
    }

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
    fs::remove_all(directory, error);
    return ok;
}

bool testFxPipelineRuntime() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;
    const bool dxc = compiler.executable().filename() == "dxc" || compiler.executable().filename() == "dxc.exe";
    if (!dxc)
        return check(true, "native FX pipeline execution requires DXC's explicit $Globals binding support");
    namespace fs = std::filesystem;
    const auto directory =
        fs::temp_directory_path() /
        ("dayo-fx-pipeline-include-" +
         std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code fileError;
    fs::create_directories(directory, fileError);
    if (fileError)
        return check(false, "FX pipeline include directory created");
    fs::create_directories(directory / "Subayai" / "hlsl", fileError);
    if (fileError)
        return check(false, "FX pipeline case-sensitive include directory created");
    {
        std::ofstream include(directory / "constants.hlsli");
        include << "#define TEST_PIPELINE_VALUE 1.0\n";
    }
    {
        std::ofstream include(directory / "Subayai" / "hlsl" / "CaseSensitive.hlsli");
        include << "#define TEST_CASE_SENSITIVE_VALUE 1.0\n";
    }
    MockDevice device;
    dayo::fx::FxProgram program;
    program.controllers = {{"Gain", "controller.pmx", "Gain", "float", {}, {}, {}}};
    dayo::core::EffectTexture outputTexture;
    outputTexture.name = "NativeOutput";
    outputTexture.format = "R8G8B8A8_UNORM";
    outputTexture.view = "UAV";
    program.textures.push_back(std::move(outputTexture));
    dayo::core::EffectTexture inputTexture;
    inputTexture.name = "NativeInput";
    inputTexture.format = "R8G8B8A8_UNORM";
    inputTexture.view = "SRV";
    program.textures.push_back(std::move(inputTexture));
    dayo::core::EffectBuffer nativeData;
    nativeData.name = "NativeData";
    nativeData.type = "float4";
    nativeData.view = "SRV";
    nativeData.elementSize = 16;
    nativeData.size.absolute = true;
    nativeData.size.width = 1;
    program.buffers.push_back(std::move(nativeData));
    dayo::core::EffectSampler nativeSampler;
    nativeSampler.name = "NativeSampler";
    program.samplers.push_back(std::move(nativeSampler));
    program.sourcePath = directory / "pipeline-runtime.fxdayo";
    program.hlsl = "#include \"constants.hlsli\"\n"
                   "#include \"subayai/hlsl/casesensitive.hlsli\"\n"
                   "#ifdef YRZ_PASS_deform\n"
                   "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { NativeOutput[id.xy] = "
                   "float4(Gain + TEST_CASE_SENSITIVE_VALUE, 0, 0, 1); }\n"
                   "#endif\n";
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "deform";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.shader = "main";
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.numThreads = {8, 4, 1};
    dispatch.macros = {"NATIVE=1"};
    dispatch.resources = {{"NativeOutput", true}};
    program.passes.push_back(dispatch);

    dayo::fx::FxNativeShaderSourceOptions sharedSource;
    sharedSource.preamble = "struct SharedValue { float4 value; };\n";
    sharedSource.resources.push_back({.declaration = "StructuredBuffer<SharedValue> SharedValues",
                                      .registerClass = dayo::fx::FxNativeShaderRegister::sampled,
                                      .registerIndex = 0,
                                      .descriptorSet = 3});
    const auto generated = dayo::fx::makeNativeFxShaderSource(program, dispatch, 7, sharedSource);
    bool ok = check(generated.find("YRZFX_ControllerCB") != std::string::npos &&
                        generated.find("#define YRZ_NUMTHREADS [numthreads(8,4,1)]") != std::string::npos &&
                        generated.find("NativeOutput : register(u0, space7)") != std::string::npos &&
                        generated.find("NativeInput : register(t0, space7)") != std::string::npos &&
                        generated.find("NativeData : register(t1, space7)") != std::string::npos &&
                        generated.find("NativeSampler : register(s0, space7)") != std::string::npos &&
                        generated.find("SharedValues : register(t0, space3)") != std::string::npos,
                    "native FX source emits disjoint typed and renderer-shared register classes");

    const auto materialTemplatePath = directory / "material-template.txt";
    {
        std::ofstream materialTemplate(materialTemplatePath);
        materialTemplate << "f.1 : Roughness\n_T2m : AlbedoMap\n_T0m : NormalMap\n_V1 : VolumeMap\n";
    }
    auto materialProgram = program;
    materialProgram.materialDescriptor = dayo::core::EffectMaterialDescriptor{
        .name = "Surface", .templatePath = "material-template.txt", .defaultFile = {}};
    materialProgram.hlsl = "#ifdef YRZ_PASS_deform\n"
                           "[numthreads(8, 4, 1)] void main(uint3 id : SV_DispatchThreadID) { "
                           "SurfaceTexture surface = GetSurfaceTexture(0, 0); "
                           "SurfaceTexture3D volume = GetSurfaceTexture3D(0, 0); "
                           "float4 albedo = surface.hasAlbedoMap ? "
                           "surface.AlbedoMap.Load(int3(0, 0, 0)) : float4(1, 0, 1, 1); "
                           "float4 volumeValue = volume.hasVolumeMap ? "
                           "volume.VolumeMap.Load(int4(0, 0, 0, 0)) : float4(0, 0, 0, 0); "
                           "NativeOutput[id.xy] = albedo + volumeValue; }\n"
                           "#endif\n";
    const auto materialGenerated = dayo::fx::makeNativeFxShaderSource(materialProgram, dispatch, 7, sharedSource);
    ok &= check(materialGenerated.find("uint tidx = imat * 3;") != std::string::npos &&
                    materialGenerated.find("_tex[tidx + 2]") != std::string::npos &&
                    materialGenerated.find("_tex[tidx + 0]") != std::string::npos &&
                    materialGenerated.find("_tex3D[tidx + 1]") != std::string::npos &&
                    materialGenerated.find("SurfaceTexture result;\n") != std::string::npos &&
                    materialGenerated.find("SurfaceTexture result = (SurfaceTexture)0;") == std::string::npos,
                "MatDesc HLSL uses declared texture slots rather than declaration traversal order");
    dayo::fx::FxShaderCompileRequest materialCompileRequest;
    materialCompileRequest.sourcePath = directory / "matdesc-generated.hlsl";
    materialCompileRequest.entryPoint = "main";
    materialCompileRequest.stage = dayo::fx::FxShaderStage::compute;
    materialCompileRequest.includeDirectories.push_back(directory);
    materialCompileRequest.hlsl = "#define YRZ_PASS_deform 1\n"
                                  "#define NonUniformResourceIndex(value) (value)\n" +
                                  materialGenerated;
    const auto materialArtifact = compiler.compile(materialCompileRequest);
    ok &= check(!materialArtifact.spirv.empty() && materialArtifact.spirv.front() == 0x07230203U,
                "MatDesc 2D and 3D getters with texture loads compile to SPIR-V");
    auto twoDimensionalMaterialProgram = materialProgram;
    twoDimensionalMaterialProgram.materialSchema =
        dayo::core::fx::parseMaterialTemplateSchema("f.1 : Roughness\n_T0 : Albedo\n", "Surface");
    twoDimensionalMaterialProgram.hlsl =
        "#ifdef YRZ_PASS_deform\n"
        "[numthreads(8, 4, 1)] void main(uint3 id : SV_DispatchThreadID) { "
        "SurfaceTexture surface = GetSurfaceTexture(0, 0); "
        "float4 albedo = surface.hasAlbedoMap ? surface.AlbedoMap.Load(int3(0, 0, 0)) : float4(1, 0, 1, 1); "
        "NativeOutput[id.xy] = albedo; }\n"
        "#endif\n";
    const auto twoDimensionalMaterialSource =
        dayo::fx::makeNativeFxShaderSource(twoDimensionalMaterialProgram, dispatch, 7, sharedSource);
    ok &= check(twoDimensionalMaterialSource.find("struct SurfaceTexture3D") == std::string::npos &&
                    twoDimensionalMaterialSource.find("GetSurfaceTexture3D") == std::string::npos,
                "MatDesc shader generation omits empty texture-object structs and accessors");

    auto deformDispatch = dispatch;
    deformDispatch.category = dayo::core::fx::FxCategory::deform;
    deformDispatch.numThreads = {};
    const auto deformGenerated = dayo::fx::makeNativeFxShaderSource(program, deformDispatch, 7, sharedSource);
    ok &= check(deformGenerated.find("#define YRZ_NUMTHREADS [numthreads(1024,1,1)]") != std::string::npos,
                "native FX source uses the upstream one-dimensional default thread group");

    dayo::core::EffectTexture depthTexture;
    depthTexture.name = "ZBuf";
    depthTexture.format = "D32_FLOAT";
    depthTexture.view = "DSV";
    program.textures.push_back(depthTexture);
    auto depthDispatch = dispatch;
    depthDispatch.resources = {{.name = "ZBuf", .write = true, .role = dayo::fx::FxResourceRole::depthAttachment}};
    const auto depthGenerated = dayo::fx::makeNativeFxShaderSource(program, depthDispatch, 7, sharedSource);
    ok &= check(depthGenerated.find("Texture2D<float> ZBuf;") != std::string::npos &&
                    depthGenerated.find("ZBuf : register(") == std::string::npos,
                "depth attachments stay out of sampled descriptor plans");

    dayo::graphics::FxPipelineRuntime runtime;
    std::string error;
    const bool built = runtime.build(
        device, program, compiler,
        [](const dayo::fx::FxDispatch&) {
            return std::optional<dayo::graphics::handles::PipelineLayoutHandle>{{1, 1}};
        },
        &error, 7, sharedSource);
    ok &= check(built, "FX pipeline runtime materializes a compute pipeline");
    ok &= check(error.empty() && runtime.size() == 1 && runtime.resolvePipeline(dispatch).has_value(),
                "FX pipeline runtime indexes the materialized pipeline");
    runtime.reset();
    ok &= check(device.destroyedPipelines_ == 1 && device.destroyedShaders_ == 1,
                "FX pipeline runtime destroys owned Vulkan objects");

    dayo::fx::FxDispatch postprocess;
    postprocess.name = "postprocess";
    postprocess.kind = dayo::fx::FxOpKind::postprocess;
    postprocess.executable = dayo::fx::FxPostProcessDispatch{"main", {}};
    program.passes = {postprocess};
    program.hlsl = "#include \"constants.hlsli\"\n"
                   "#ifdef YRZ_PASS_postprocess\n"
                   "float4 main() : SV_Target { return float4(TEST_PIPELINE_VALUE, 0, 0, 1); }\n"
                   "#endif\n";
    ok &= check(runtime.build(
                    device, program, compiler,
                    [](const dayo::fx::FxDispatch&) {
                        return std::optional<dayo::graphics::handles::PipelineLayoutHandle>{{1, 1}};
                    },
                    &error) &&
                    error.empty() && runtime.resolvePipeline(postprocess).has_value(),
                "FX pipeline runtime combines renderer fullscreen vertex with postprocess pixel shader");
    runtime.reset();
    fs::remove_all(directory, fileError);
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
    ok &= testResolvedPassPlanning();
    ok &= testRasterModelTargetIndexedDraws();
    ok &= testBufferlessIndexRasterExecution();
    ok &= testFxVertexBufferRasterExecution();
    ok &= testDepthOnlyRasterExecution();
    ok &= testOidnHostExecution();
    ok &= testOidnStructuredBufferInput();
    ok &= testTypedBufferResourceExecution();
    ok &= testBufferClearUsesFunctionalValue();
    ok &= testDayoHostResourceProvider();
    ok &= testNativeSceneDerivedResources();
    ok &= testViewConstantsAndScreenHistory();
    ok &= testFxControllerResolver();
    ok &= testFxConditionRuntime();
    ok &= testNativeFxGlobalVariableRuntime();
    ok &= testFxMaterialGpuRuntimeOwnsTablesAndTextures();
    ok &= testFxMaterialSceneRuntimeEvaluatesProjectAnnotations();
    ok &= testPreviewReferencePath();
    ok &= testSchedulerOrder();
    ok &= testCloneUnification();
    ok &= testWatcherReverseDeps();
    ok &= testHotReloadKeepsCurrentOnFailure();
    ok &= testCompilerUsesRawSourceAndRejectsUnknownPasses();
    ok &= testRayTracingPayloadIsLossless();
    ok &= testFxResourceDeclarationsAreLossless();
    ok &= testFxResourceRuntimeMaterializesDeclarations();
    ok &= testDepthTextureUsageIncludesSampling();
    ok &= testFxExternalTextureMetadataAndUpload();
    ok &= testNativeFxRuntimeRefreshesFrameResources();
    ok &= testNativeFxRuntimeBindsResourcesAndPipelines();
    ok &= testNativeFxRuntimeBindsFixedSceneSets();
    ok &= testNativeFxRuntimeExecutesMatDescDescriptorSets();
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
