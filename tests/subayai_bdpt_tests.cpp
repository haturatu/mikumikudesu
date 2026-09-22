#include "core/denoiser.hpp"
#include "core/image_hdr.hpp"
#include "core/scene.hpp"
#include "graphics/bdpt_accumulation.hpp"
#include "graphics/bdpt_runtime.hpp"
#include "graphics/dayo_host_resources.hpp"
#include "graphics/device.hpp"
#include "graphics/native_controller_runtime.hpp"
#include "graphics/native_frame_constants.hpp"
#include "graphics/native_scene_binding_runtime.hpp"
#include "graphics/native_scene_bindings.hpp"
#include "graphics/native_scene_data.hpp"
#include "graphics/native_scene_frame_runtime.hpp"
#include "graphics/native_scene_model_runtime.hpp"
#include "graphics/native_scene_resource_runtime.hpp"
#include "graphics/native_scene_resource_store.hpp"
#include "graphics/sbt.hpp"
#include "graphics/subayai_acceleration_structure.hpp"
#include "graphics/subayai_deform.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/subayai_geometry.hpp"
#include "graphics/subayai_light_sampling.hpp"
#include "graphics/subayai_material_gpu.hpp"
#include "graphics/subayai_material_runtime.hpp"
#include "graphics/subayai_runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

bool check(bool value, std::string_view message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}

bool supportsRayGeneration(const dayo::fx::FxShaderCompiler& compiler) {
    dayo::fx::FxShaderCompileRequest request;
    request.hlsl = "void main() {}\n";
    request.stage = dayo::fx::FxShaderStage::rayGeneration;
    try {
        return !compiler.compile(request).spirv.empty();
    } catch (...) {
        return false;
    }
}

struct MockAccelerationBackend : dayo::graphics::IAccelerationBackend {
    std::uint32_t next{1};
    std::uint64_t createBlasCalls{0};
    std::uint64_t rebuildBlasCalls{0};
    std::uint64_t refitBlasCalls{0};
    std::uint64_t createTlasCalls{0};
    std::uint64_t rebuildTlasCalls{0};
    std::uint64_t updateTlasCalls{0};
    std::uint64_t recordBlasCalls{0};
    std::uint64_t recordTlasCalls{0};
    std::uint64_t destroyBlasCalls{0};
    std::uint64_t destroyTlasCalls{0};
    bool replaceTlasOnRebuild{false};
    std::size_t lastTlasInstanceCount{};
    std::vector<dayo::graphics::handles::BufferHandle> rebuildVertexBuffers;
    std::vector<dayo::graphics::handles::BufferHandle> refitVertexBuffers;
    std::vector<dayo::graphics::handles::BufferHandle> recordedVertexBuffers;
    std::vector<dayo::graphics::TlasInstanceDesc> lastTlasInstances;
    std::vector<std::string>* commandEvents{};

    dayo::graphics::handles::AccelerationStructureHandle
    createBlas(const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++createBlasCalls;
        if (geometry.triangles.empty())
            return {};
        return {next++, 1};
    }
    dayo::graphics::handles::AccelerationStructureHandle
    rebuildBlas(dayo::graphics::handles::AccelerationStructureHandle blas,
                const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++rebuildBlasCalls;
        rebuildVertexBuffers.push_back(geometry.triangles.front().vertexBuffer);
        return blas;
    }
    void refitBlas(dayo::graphics::handles::AccelerationStructureHandle,
                   const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++refitBlasCalls;
        refitVertexBuffers.push_back(geometry.triangles.front().vertexBuffer);
    }
    dayo::graphics::handles::AccelerationStructureHandle
    createTlas(std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++createTlasCalls;
        lastTlasInstanceCount = instances.size();
        lastTlasInstances.assign(instances.begin(), instances.end());
        return {next++, 1};
    }
    dayo::graphics::handles::AccelerationStructureHandle
    rebuildTlas(dayo::graphics::handles::AccelerationStructureHandle tlas,
                std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++rebuildTlasCalls;
        lastTlasInstanceCount = instances.size();
        lastTlasInstances.assign(instances.begin(), instances.end());
        return replaceTlasOnRebuild ? dayo::graphics::handles::AccelerationStructureHandle{next++, 1} : tlas;
    }
    void updateTlas(dayo::graphics::handles::AccelerationStructureHandle,
                    std::span<const dayo::graphics::TlasInstanceDesc> instances) override {
        ++updateTlasCalls;
        lastTlasInstanceCount = instances.size();
        lastTlasInstances.assign(instances.begin(), instances.end());
    }
    void recordBlasUpdate(dayo::graphics::CommandList&, dayo::graphics::handles::AccelerationStructureHandle,
                          const dayo::graphics::BlasGeometryDesc& geometry) override {
        ++recordBlasCalls;
        recordedVertexBuffers.push_back(geometry.triangles.front().vertexBuffer);
        if (commandEvents != nullptr)
            commandEvents->emplace_back("blas");
    }
    void recordTlasUpdate(dayo::graphics::CommandList&, dayo::graphics::handles::AccelerationStructureHandle,
                          std::span<const dayo::graphics::TlasInstanceDesc>) override {
        ++recordTlasCalls;
        if (commandEvents != nullptr)
            commandEvents->emplace_back("tlas");
    }
    void destroyBlas(dayo::graphics::handles::AccelerationStructureHandle) override {
        ++destroyBlasCalls;
    }
    void destroyTlas(dayo::graphics::handles::AccelerationStructureHandle) override {
        ++destroyTlasCalls;
    }
};

struct MockEnvironmentBackend : dayo::graphics::IEnvironmentBackend {
    std::uint64_t regenerations{0};
    mutable std::uint64_t recordings{0};
    void regenerate(const dayo::graphics::EnvironmentDesc&) override {
        ++regenerations;
    }
    void record(dayo::graphics::CommandList&) const override {
        ++recordings;
    }
};

struct MockNativeDevice final : dayo::graphics::Device {
    struct Buffer {
        std::vector<std::byte> bytes;
    };

    const dayo::graphics::DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    const dayo::graphics::GraphicsConvention& convention() const noexcept override {
        return convention_;
    }
    dayo::graphics::RendererKind activeRenderer() const noexcept override {
        return dayo::graphics::RendererKind::subayai;
    }
    void selectRenderer(dayo::graphics::RendererKind) override {}
    void resize() override {}
    void beginUiFrame() override {}
    void renderFrame() override {}
    std::size_t currentFrameSlot() const noexcept override {
        return frameSlot;
    }
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
        return nextHandle_++;
    }
    dayo::graphics::TextureHandle createTexture(const dayo::graphics::TextureDesc&) override {
        return nextHandle_++;
    }
    dayo::graphics::handles::DescriptorSetLayoutHandle
    createDescriptorSetLayoutEx(const dayo::graphics::DescriptorSetLayoutDesc& desc) override {
        lastDescriptorLayout = desc;
        return {nextDescriptorLayout_++, 1};
    }
    dayo::graphics::handles::TextureHandle createTextureEx(const dayo::graphics::TextureResourceDesc&) override {
        return {nextTypedTexture_++, 1};
    }
    dayo::graphics::handles::BufferHandle createBufferEx(const dayo::graphics::BufferResourceDesc& desc) override {
        const auto handle = dayo::graphics::handles::BufferHandle{nextTypedBuffer_++, 1};
        typedBuffers_.emplace(handle, Buffer{std::vector<std::byte>(desc.size)});
        typedBufferUsages.emplace_back(handle, desc.usage);
        return handle;
    }
    void destroyTextureEx(dayo::graphics::handles::TextureHandle handle) override {
        if (handle.valid())
            ++destroyedTextures;
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle, const std::array<float, 4>&) override {
        ++clearedTextures;
    }
    void destroyBufferEx(dayo::graphics::handles::BufferHandle handle) override {
        ++destroyedBuffers;
        typedBuffers_.erase(handle);
    }
    void copyBufferEx(dayo::graphics::handles::BufferHandle source,
                      dayo::graphics::handles::BufferHandle destination) override {
        const auto sourceIt = typedBuffers_.find(source);
        const auto destinationIt = typedBuffers_.find(destination);
        if (sourceIt == typedBuffers_.end() || destinationIt == typedBuffers_.end() ||
            sourceIt->second.bytes.size() > destinationIt->second.bytes.size())
            throw std::out_of_range("mock typed buffer copy exceeds allocation");
        std::copy(sourceIt->second.bytes.begin(), sourceIt->second.bytes.end(), destinationIt->second.bytes.begin());
    }
    dayo::graphics::handles::DescriptorSetHandle
    allocateDescriptorSetEx(dayo::graphics::handles::DescriptorSetLayoutHandle layout,
                            std::span<const dayo::graphics::DescriptorBindingEx> bindings) override {
        if (!layout.valid())
            throw std::invalid_argument("mock descriptor layout is invalid");
        lastDescriptorBindings.assign(bindings.begin(), bindings.end());
        const auto set = dayo::graphics::handles::DescriptorSetHandle{nextDescriptorSet_++, 1};
        descriptorBindings_[set].assign(bindings.begin(), bindings.end());
        return set;
    }
    void updateDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle set,
                               std::span<const dayo::graphics::DescriptorBindingEx> bindings) override {
        lastDescriptorBindings.assign(bindings.begin(), bindings.end());
        descriptorBindings_[set].assign(bindings.begin(), bindings.end());
    }
    void destroyDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle handle) override {
        if (handle.valid()) {
            ++destroyedDescriptorSets;
            descriptorBindings_.erase(handle);
        }
    }
    void destroyDescriptorSetLayoutEx(dayo::graphics::handles::DescriptorSetLayoutHandle handle) override {
        if (handle.valid())
            ++destroyedDescriptorLayouts;
    }
    dayo::graphics::handles::PipelineLayoutHandle
    createPipelineLayoutEx(const dayo::graphics::PipelineLayoutDesc&) override {
        return {nextPipelineLayout_++, 1};
    }
    void destroyPipelineLayoutEx(dayo::graphics::handles::PipelineLayoutHandle) override {}
    dayo::graphics::handles::ShaderHandle createShaderEx(const dayo::graphics::ShaderDesc&) override {
        return {nextShader_++, 1};
    }
    void destroyShaderEx(dayo::graphics::handles::ShaderHandle) override {}
    dayo::graphics::handles::PipelineHandle
    createComputePipelineEx(const dayo::graphics::ComputePipelineDescEx&) override {
        return {nextPipeline_++, 1};
    }
    void destroyPipelineEx(dayo::graphics::handles::PipelineHandle) override {}
    dayo::graphics::handles::PipelineHandle
    createRayTracingPipelineEx(const dayo::graphics::RayTracingPipelineDescEx&) override {
        return {nextPipeline_++, 1};
    }
    dayo::graphics::handles::ShaderBindingTableHandle
    createShaderBindingTable(const dayo::graphics::ShaderBindingTableDesc&) override {
        return {nextSbt_++, 1};
    }
    void destroyShaderBindingTable(dayo::graphics::handles::ShaderBindingTableHandle) override {}
    void uploadTextureEx(dayo::graphics::handles::TextureHandle, std::span<const std::uint8_t>, std::uint32_t,
                         std::uint32_t) override {}
    void uploadBufferEx(dayo::graphics::handles::BufferHandle handle, std::span<const std::byte> bytes,
                        std::size_t offset) override {
        ++uploadBufferCalls;
        auto it = typedBuffers_.find(handle);
        if (it == typedBuffers_.end() || offset > it->second.bytes.size() ||
            bytes.size() > it->second.bytes.size() - offset)
            throw std::out_of_range("mock typed buffer upload exceeds allocation");
        std::copy(bytes.begin(), bytes.end(), it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    std::vector<std::byte> readbackBufferEx(dayo::graphics::handles::BufferHandle handle, std::size_t offset,
                                            std::size_t size) override {
        const auto it = typedBuffers_.find(handle);
        if (it == typedBuffers_.end() || offset > it->second.bytes.size() || size > it->second.bytes.size() - offset)
            throw std::out_of_range("mock typed buffer readback exceeds allocation");
        return {it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }

    dayo::graphics::DeviceCapabilities capabilities_{
        .gpuName = "mock",
        .driverName = "mock",
        .swapchain = true,
        .bufferDeviceAddress = true,
        .descriptorIndexing = true,
        .accelerationStructure = true,
        .rayTracingPipeline = true,
        .rayQuery = true,
        .fragmentShaderBarycentric = true,
    };
    dayo::graphics::GraphicsConvention convention_;
    dayo::graphics::BufferHandle nextHandle_{1};
    std::uint32_t nextTypedBuffer_{1};
    std::uint32_t nextTypedTexture_{1};
    std::uint32_t nextDescriptorLayout_{1};
    std::uint32_t nextDescriptorSet_{1};
    std::uint32_t nextPipelineLayout_{1};
    std::uint32_t nextShader_{1};
    std::uint32_t nextPipeline_{1};
    std::uint32_t nextSbt_{1};
    std::size_t destroyedBuffers{};
    std::size_t destroyedTextures{};
    std::size_t clearedTextures{};
    std::size_t destroyedDescriptorSets{};
    std::size_t destroyedDescriptorLayouts{};
    std::size_t uploadBufferCalls{};
    std::size_t frameSlot{};
    dayo::graphics::DescriptorSetLayoutDesc lastDescriptorLayout;
    std::vector<dayo::graphics::DescriptorBindingEx> lastDescriptorBindings;
    std::vector<std::pair<dayo::graphics::handles::BufferHandle, dayo::graphics::ResourceUsage>> typedBufferUsages;
    std::unordered_map<dayo::graphics::handles::DescriptorSetHandle, std::vector<dayo::graphics::DescriptorBindingEx>>
        descriptorBindings_;
    std::unordered_map<dayo::graphics::handles::BufferHandle, Buffer> typedBuffers_;
};

struct MockNativeSceneCommands final : dayo::graphics::CommandList {
    explicit MockNativeSceneCommands(MockNativeDevice& device) : device_(&device) {}

    void transition(dayo::graphics::TextureHandle) override {}
    void bindPipeline(dayo::graphics::PipelineHandle) override {}
    void draw(std::uint32_t, std::uint32_t) override {}
    void dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void traceRays(std::uint32_t, std::uint32_t) override {}
    void copyBufferEx(dayo::graphics::handles::BufferHandle source,
                      dayo::graphics::handles::BufferHandle destination) override {
        copies.emplace_back(source, destination);
        events.emplace_back("copy");
        device_->copyBufferEx(source, destination);
    }
    void transferBarrierEx() override {
        events.emplace_back("transfer-barrier");
        transferBarrierRecorded = true;
    }
    void memoryBarrierEx() override {
        events.emplace_back("memory-barrier");
        barrierRecorded = true;
    }

    MockNativeDevice* device_{};
    std::vector<std::pair<dayo::graphics::handles::BufferHandle, dayo::graphics::handles::BufferHandle>> copies;
    std::vector<std::string> events;
    bool barrierRecorded{};
    bool transferBarrierRecorded{};
};

struct MockDeformCommands final : dayo::graphics::CommandList {
    std::vector<std::string> events;
    std::vector<std::byte> constants;
    std::vector<std::pair<dayo::graphics::handles::DescriptorSetHandle, std::uint32_t>> descriptorSets;

    void transition(dayo::graphics::TextureHandle) override {}
    void bindPipeline(dayo::graphics::PipelineHandle) override {}
    void draw(std::uint32_t, std::uint32_t) override {}
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override {
        events.push_back("dispatch:" + std::to_string(x) + "x" + std::to_string(y) + "x" + std::to_string(z));
    }
    void traceRays(std::uint32_t, std::uint32_t) override {}
    void uploadBufferEx(dayo::graphics::handles::BufferHandle, std::span<const std::byte>, std::size_t) override {
        events.emplace_back("upload");
    }
    void traceRaysEx(dayo::graphics::handles::PipelineHandle, dayo::graphics::handles::ShaderBindingTableHandle,
                     std::uint32_t, std::uint32_t, std::uint32_t) override {
        events.emplace_back("traceEx");
    }
    void transitionEx(dayo::graphics::handles::TextureHandle) override {
        events.emplace_back("transition");
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle) override {
        events.emplace_back("clear");
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle, const std::array<float, 4>&) override {
        events.emplace_back("clear");
    }
    void bindPipelineEx(dayo::graphics::handles::PipelineHandle) override {
        events.emplace_back("bind");
    }
    void bindDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle set, std::uint32_t setIndex) override {
        descriptorSets.emplace_back(set, setIndex);
        events.emplace_back("descriptor");
    }
    void pushConstantsEx(std::span<const std::byte> bytes) override {
        constants.assign(bytes.begin(), bytes.end());
        events.emplace_back("push");
    }
    void memoryBarrierEx() override {
        events.emplace_back("barrier");
    }
    void generateMipmapsEx(dayo::graphics::handles::TextureHandle) override {
        events.emplace_back("mipmap");
    }
    void accelerationStructureBarrierEx() override {
        events.emplace_back("as-barrier");
    }
};

bool testSubayaiNativeFxExecution() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;

    MockNativeDevice device;
    dayo::graphics::SubayaiRuntime runtime;
    dayo::fx::FxProgram program;
    program.label = "Subayai-native";
    program.sourcePath = "subayai-native.fxdayo";
    program.hlsl = "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {}\n";
    dayo::core::EffectTexture output;
    output.name = "Output";
    output.view = "UAV";
    program.textures.push_back(std::move(output));
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "native-compute";
    dispatch.kind = dayo::fx::FxOpKind::compute;
    dispatch.executable = dayo::fx::FxComputeDispatch{"main"};
    dispatch.resources.push_back({"Output", true});
    program.passes.push_back(std::move(dispatch));

    std::string error;
    bool ok = check(runtime.initialize(device, std::move(program), &error),
                    "Subayai runtime accepts a compilable native FX program");
    const auto context = dayo::fx::makeFxFrameContext(0.0F, 0, 16, 8, 1, 0, 3, 1, 1, 1);
    dayo::core::MaterialParameterBlock material;
    material.set("Anisotropy", 0.75F);
    const std::array<dayo::core::MaterialParameterBlock, 1> materials{material};
    const std::array<dayo::graphics::AliasEntry, 1> lightSampling{{{1.0F, 0}}};
    dayo::graphics::EnvironmentGpuResult environment;
    environment.cubemap = {40, 1};
    environment.prefiltered = {41, 1};
    environment.sphericalHarmonics[0] = 1.0F;
    auto frame = runtime.prepareFrame(context, materials, lightSampling, environment);
    ok &=
        check(runtime.nativeReady() && frame.nativeFx.has_value(), "Subayai runtime prepares the native FX frame path");
    const auto frameOutput = runtime.output(frame);
    ok &= check(frameOutput.has_value() && frameOutput->valid() && frameOutput->extent.width == context.renderWidth &&
                    frameOutput->extent.height == context.renderHeight &&
                    frameOutput->format == dayo::graphics::PixelFormat::rgba8Unorm,
                "Subayai runtime exposes the last typed texture as its frame output");
    ok &= check(frame.environmentDescriptorSet.valid(), "Subayai frame exposes an environment descriptor set");
    MockDeformCommands commands;
    const auto stats = runtime.execute(frame, commands);
    ok &= check(stats.compute == 1 &&
                    commands.events == std::vector<std::string>{"transition", "descriptor", "descriptor", "descriptor",
                                                                "descriptor", "bind", "dispatch:1x1x1", "barrier"},
                "Subayai runtime executes typed FX resources through the native path");
    ok &= check(commands.descriptorSets.size() == 4 && commands.descriptorSets[0].second == 0 &&
                    commands.descriptorSets[1].second == 1 && commands.descriptorSets[2].second == 2 &&
                    commands.descriptorSets[3].second == 3 &&
                    commands.descriptorSets[0].first == frame.materialDescriptorSet &&
                    commands.descriptorSets[1].first == frame.lightSamplingDescriptorSet &&
                    commands.descriptorSets[2].first == frame.environmentDescriptorSet,
                "Subayai native FX binds material, light, environment, and effect resources in stable set order");
    runtime.reset();
    return ok;
}

bool testBdptNativeFxExecution() {
    dayo::fx::FxShaderCompiler compiler;
    if (!compiler.available())
        return true;
    // Ubuntu's fallback glslc package may compile the raster/compute stages
    // used by the other native tests without supporting ray-generation input.
    // Keep this test active on RT-capable toolchains and let the runtime
    // capability query provide the production fallback on older toolchains.
    if (!supportsRayGeneration(compiler))
        return true;

    MockNativeDevice device;
    dayo::graphics::BdptRuntime runtime;
    dayo::fx::FxProgram program;
    program.label = "BDPT-native";
    program.sourcePath = "bdpt-native.fxdayo";
    program.hlsl = "void main() {}\n";
    dayo::fx::FxDispatch dispatch;
    dispatch.name = "native-raytrace";
    dispatch.kind = dayo::fx::FxOpKind::raytracing;
    dayo::fx::FxRayTracingDispatch ray;
    ray.rayGenerationShader = "main";
    ray.maxPayloadSize = 32;
    ray.maxAttributeSize = 8;
    ray.maxRecursionDepth = 1;
    dispatch.executable = std::move(ray);
    program.passes.push_back(std::move(dispatch));

    std::string error;
    bool ok = check(runtime.initialize(device, std::move(program), &error),
                    "BDPT runtime accepts a compilable native RT program");
    const auto context = dayo::fx::makeFxFrameContext(0.0F, 0, 16, 8, 1, 0, 3, 1, 1, 1);
    const std::array<dayo::graphics::AliasEntry, 1> lightSampling{{{1.0F, 0}}};
    auto frame = runtime.prepareFrame(context, dayo::core::DirtyFlag::geometry, lightSampling);
    ok &= check(runtime.nativeReady() && frame.nativeFx.has_value(), "BDPT runtime prepares the native FX frame path");
    if (!runtime.nativeReady() || !frame.nativeFx.has_value()) {
        runtime.reset();
        return false;
    }
    const auto frameOutput = runtime.output(frame);
    ok &= check(frameOutput.has_value() && frameOutput->texture == frame.gpu.accumulation &&
                    frameOutput->extent.width == context.renderWidth &&
                    frameOutput->extent.height == context.renderHeight &&
                    frameOutput->format == dayo::graphics::PixelFormat::rgba16Float,
                "BDPT runtime exposes persistent accumulation when the graph has no explicit output");
    MockDeformCommands commands;
    const auto stats = runtime.execute(frame, commands);
    ok &= check(stats.rayTracing == 1 &&
                    commands.events == std::vector<std::string>{"transition", "clear", "barrier", "descriptor",
                                                                "descriptor", "bind", "traceEx"},
                "BDPT runtime clears accumulation and executes the native RT pass");
    ok &= check(commands.descriptorSets.size() == 2 && commands.descriptorSets[0].second == 0 &&
                    commands.descriptorSets[0].first == frame.lightSamplingDescriptorSet &&
                    commands.descriptorSets[1].second == 1 && commands.descriptorSets[1].first == frame.descriptorSet,
                "BDPT native FX binds light sampling before persistent resources");
    runtime.reset();
    return ok;
}

} // namespace

dayo::graphics::BlasGeometryDesc geometry(std::uint32_t vertexBuffer) {
    return {.triangles = {{.vertexBuffer = {vertexBuffer, 1},
                           .vertexStride = sizeof(dayo::graphics::NativeDeformedVertex),
                           .vertexCount = 3,
                           .indexBuffer = {99, 1},
                           .indexType = dayo::graphics::IndexType::uint32,
                           .indexCount = 3}}};
}

int main() {
    using dayo::graphics::AccelerationStructureService;
    using dayo::graphics::AliasEntry;
    using dayo::graphics::BdptAccumulation;
    using dayo::graphics::BlasAction;
    using dayo::graphics::EnvironmentDesc;
    using dayo::graphics::EnvironmentService;
    using dayo::graphics::LightSamplingService;
    using dayo::graphics::ShaderBindingTableBuilder;
    using dayo::graphics::TlasAction;
    bool ok = true;

    ok &= testSubayaiNativeFxExecution();
    ok &= testBdptNativeFxExecution();

    // Native material linking keeps Subayai hair controls in a dedicated GPU
    // ABI while leaving PreviewMaterialGpu untouched.
    {
        dayo::core::MaterialParameterBlock parameters;
        parameters.set("BaseColor", std::array<float, 4>{0.2F, 0.3F, 0.4F, 1.0F});
        parameters.set("Anisotropy", 0.75F);
        parameters.set("IOR", std::array<float, 2>{1.45F, 0.12F});
        parameters.set("AutoNormal", 1.0F);
        parameters.set("Roughness", 0.25F);
        const auto gpu = dayo::graphics::linkSubayaiMaterial(parameters);
        ok &= check(gpu.baseColor[0] == 0.2F && gpu.baseColor[3] == 1.0F, "Subayai base color links to GPU ABI");
        ok &= check(gpu.hair[0] == 0.75F && gpu.hair[1] == 1.45F && gpu.hair[2] == 0.12F && gpu.hair[3] == 1.0F,
                    "Subayai hair anisotropy IOR and AutoNormal link to GPU ABI");
        ok &= check(gpu.surface[0] == 0.25F, "Subayai roughness links to native material ABI");
    }

    // A changed material generation must be uploaded to every frame slot,
    // including a slot that already carried the previous generation.
    {
        MockNativeDevice device;
        dayo::graphics::SubayaiMaterialGpuRuntime runtime;
        dayo::core::MaterialParameterBlock initial;
        initial.set("BaseColor", std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F});
        const std::array<dayo::core::MaterialParameterBlock, 1> initialMaterials{initial};
        std::string error;
        ok &= check(runtime.sync(device, initialMaterials, &error),
                    "Subayai material runtime initializes both frame slots");
        const auto slot0Buffer = runtime.buffer();
        auto changed = initial;
        changed.set("BaseColor", std::array<float, 4>{0.9F, 0.8F, 0.7F, 1.0F});
        const std::array<dayo::core::MaterialParameterBlock, 1> changedMaterials{changed};
        device.frameSlot = 0;
        ok &= check(runtime.sync(device, changedMaterials, &error),
                    "Subayai material runtime uploads a changed generation to slot zero");
        device.frameSlot = 1;
        ok &= check(runtime.sync(device, changedMaterials, &error),
                    "Subayai material runtime uploads the same changed generation to slot one");
        const auto slot1Buffer = runtime.buffer();
        const auto expected = dayo::graphics::linkSubayaiMaterial(changed);
        const auto readMaterial = [&](const auto buffer) {
            dayo::graphics::SubayaiMaterialGpu value{};
            const auto bytes = device.readbackBufferEx(buffer, 0, sizeof(value));
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        };
        const auto slot0 = readMaterial(slot0Buffer);
        const auto slot1 = readMaterial(slot1Buffer);
        const auto materialMatches = [&expected](const auto& value) {
            return value.baseColor[0] == expected.baseColor[0] && value.baseColor[1] == expected.baseColor[1] &&
                   value.baseColor[2] == expected.baseColor[2] && value.baseColor[3] == expected.baseColor[3];
        };
        ok &= check(materialMatches(slot0) && materialMatches(slot1),
                    "Subayai material slots contain the current generation");
    }

    // Feature requirements are derived from the compiled graph, not from a
    // renderer name alone.
    {
        dayo::fx::FxProgram subayaiProgram;
        subayaiProgram.passes.push_back({"compute", dayo::fx::FxOpKind::compute, "cs", 1, 1, {}, {}, {}, {}});
        const auto subayaiRequired = dayo::fx::requiredFeatures(subayaiProgram);
        ok &= check(!subayaiRequired.rayTracingPipeline, "compute Subayai graph does not require RT pipeline");
        dayo::fx::FxProgram rtProgram;
        rtProgram.passes.push_back(
            {"rt", dayo::fx::FxOpKind::raytracing, {}, 1, 1, {}, {}, dayo::fx::FxRayTracingDispatch{}, {}});
        const auto rtRequired = dayo::fx::requiredFeatures(rtProgram);
        ok &= check(rtRequired.accelerationStructure && rtRequired.rayQuery && rtRequired.rayTracingPipeline,
                    "RT graph reports its Vulkan feature requirements");
    }

    // A graph with only compute work can be prepared on RT-capable hardware
    // even while DeviceCapabilities native flags remain owned by the runtime.
    {
        MockNativeDevice device;
        dayo::graphics::SubayaiRuntime runtime;
        dayo::fx::FxProgram program;
        program.label = "Subayai";
        program.passes.push_back({"compute", dayo::fx::FxOpKind::compute, "cs", 1, 1, {}, {}, {}, {}});
        std::string error;
        ok &= check(runtime.initialize(device, program, &error), "Subayai runtime initializes on supported hardware");
        dayo::core::MaterialParameterBlock parameters;
        parameters.set("Anisotropy", 1.0F);
        ok &= check(runtime.syncMaterials(std::span<const dayo::core::MaterialParameterBlock>(&parameters, 1)),
                    "Subayai runtime links material parameters");
        const std::array<dayo::graphics::AliasEntry, 2> lightTable{{{0.75F, 0}, {1.0F, 1}}};
        auto frame =
            runtime.prepareFrame(dayo::fx::makeFxFrameContext(0.0F, 0, 64, 32, 1, 0, 3, 1, 1, 1),
                                 std::span<const dayo::core::MaterialParameterBlock>(&parameters, 1), lightTable, {});
        ok &= check(frame.plan.ordered.size() == 1 && frame.materials.size() == 1,
                    "Subayai runtime prepares graph and material frame state");
        ok &= check(frame.materialBuffer.valid(), "Subayai frame exposes a typed material storage buffer");
        ok &= check(frame.materialDescriptorSet.valid(), "Subayai frame exposes a material descriptor set");
        const auto materialBytes =
            device.readbackBufferEx(frame.materialBuffer, 0, sizeof(dayo::graphics::SubayaiMaterialGpu));
        ok &= check(materialBytes.size() == sizeof(dayo::graphics::SubayaiMaterialGpu),
                    "Subayai material ABI is uploaded to the typed buffer");
        ok &= check(frame.lightSamplingBuffer.valid(), "Subayai frame exposes a typed light sampling buffer");
        ok &= check(frame.lightSamplingDescriptorSet.valid(), "Subayai frame exposes a light descriptor set");
        const auto lightBytes = device.readbackBufferEx(frame.lightSamplingBuffer, 0,
                                                        lightTable.size() * sizeof(dayo::graphics::AliasEntry));
        ok &= check(lightBytes.size() == lightTable.size() * sizeof(dayo::graphics::AliasEntry),
                    "Subayai light sampling table is uploaded to the typed buffer");
        ok &= check(device.lastDescriptorBindings.size() == 1 &&
                        device.lastDescriptorBindings.front().buffer == frame.lightSamplingBuffer,
                    "Subayai light descriptor points at the uploaded buffer");
        dayo::graphics::FxExecutionResources execution;
        execution.resolveTypedPipeline =
            [](const dayo::fx::FxDispatch&) -> std::optional<dayo::graphics::handles::PipelineHandle> {
            return dayo::graphics::handles::PipelineHandle{7, 1};
        };
        MockDeformCommands commands;
        const auto stats = runtime.execute(frame, commands, execution);
        ok &= check(stats.compute == 1 && commands.events == std::vector<std::string>{"descriptor", "descriptor",
                                                                                      "bind", "dispatch:4x2x1"},
                    "Subayai execution binds both native resource descriptor sets");
        ok &= check(commands.descriptorSets.size() == 2 && commands.descriptorSets[0].second == 0 &&
                        commands.descriptorSets[1].second == 1 &&
                        commands.descriptorSets[0].first == frame.materialDescriptorSet &&
                        commands.descriptorSets[1].first == frame.lightSamplingDescriptorSet,
                    "Subayai descriptor bindings preserve pipeline set indices");
        runtime.reset();
        ok &= check(!runtime.ready(), "Subayai runtime reset disables execution");

        dayo::graphics::SubayaiRuntime rtRuntime;
        dayo::fx::FxProgram rtProgram;
        dayo::fx::FxDispatch rtDispatch;
        rtDispatch.name = "effect-declared-rt";
        rtDispatch.kind = dayo::fx::FxOpKind::raytracing;
        rtDispatch.executable = dayo::fx::FxRayTracingDispatch{};
        rtProgram.passes.push_back(std::move(rtDispatch));
        ok &= check(rtRuntime.initialize(device, std::move(rtProgram), &error),
                    "Subayai runtime accepts an effect-declared RT contract when the device supports it");
        rtRuntime.reset();
    }

    // BDPT persistent resources are real typed allocations, while the host
    // LUTs remain deterministic and inspectable in a backend-neutral test.
    {
        MockNativeDevice device;
        dayo::graphics::BdptRuntime runtime;
        dayo::fx::FxProgram program;
        program.label = "BDPT";
        program.passes.push_back({"path-trace", dayo::fx::FxOpKind::raytracing, {}, 1, 1, {}, {}, {}, {}});
        std::string error;
        ok &= check(runtime.initialize(device, program, &error), "BDPT runtime initializes on RT hardware");
        ok &= check(runtime.ensureResources(16, 8, &error), "BDPT runtime allocates persistent GPU resources");
        const auto gpu = runtime.accumulation().gpuResources();
        ok &= check(gpu.valid() && gpu.width == 16 && gpu.height == 8, "BDPT persistent handles are complete");
        ok &= check(device.clearedTextures == 1U + BdptAccumulation::kVolumeSlots,
                    "BDPT accumulation and volume resources start cleared");
        ok &= check(runtime.accumulation().spectralLut().front() != runtime.accumulation().spectralLut().back(),
                    "BDPT spectral LUT contains generated data");
        const auto lutBytes =
            device.readbackBufferEx(gpu.spectralLut, 0, runtime.accumulation().spectralLut().size() * sizeof(float));
        ok &= check(lutBytes.size() == runtime.accumulation().spectralLut().size() * sizeof(float),
                    "BDPT spectral LUT is uploaded to its typed buffer");
        const auto context = dayo::fx::makeFxFrameContext(0.0F, 0, 16, 8, 1, 0, 3, 1, 1, 1);
        auto first = runtime.prepareFrame(context, dayo::core::DirtyFlag::geometry);
        ok &= check(first.clearAccumulation && first.sampleIndex == 0 && first.context.sample == 0,
                    "BDPT dirty frame clears and publishes sample zero");
        auto second = runtime.prepareFrame(context, dayo::core::DirtyFlag::none);
        ok &= check(!second.clearAccumulation && second.sampleIndex == 1 && second.context.sample == 1,
                    "BDPT clean frame advances and publishes the sample index");
        ok &= check(first.gpu.accumulation == second.gpu.accumulation, "BDPT accumulation texture persists per extent");
        runtime.reset();
        ok &= check(!runtime.ready() && !runtime.accumulation().gpuReady(), "BDPT reset releases typed resources");
    }

    // Native deformation writes actual positions into a buffer that is valid
    // for both vertex reads and BLAS build input. Preview's source skinning
    // attributes never appear as an AS vertex buffer.
    {
        const dayo::graphics::NativeDeformInput input{
            .vertexCount = 130, .indexCount = 192, .boneCount = 8, .morphDeltaCount = 24, .morphCount = 6};
        const auto plan = dayo::graphics::makeNativeDeformPlan(input);
        ok &= check(plan.deformedVertices.size == 130U * sizeof(dayo::graphics::NativeDeformedVertex),
                    "native deform allocates deformed vertex output");
        ok &= check((dayo::graphics::toBits(plan.deformedVertices.usage) &
                     dayo::graphics::toBits(dayo::graphics::ResourceUsage::asBuildRead)) != 0U,
                    "deformed output is BLAS build-readable");
        ok &= check((dayo::graphics::toBits(plan.deformedVertices.usage) &
                     dayo::graphics::toBits(dayo::graphics::ResourceUsage::transferDst)) != 0U,
                    "deformed output accepts its initial device-local seed upload");
        ok &= check(plan.deformedVertices.lifetime == dayo::graphics::ResourceLifetime::persistent,
                    "deformed output persists across BLAS updates");
        ok &= check(plan.workgroupCount == 3, "native deform rounds dispatch groups up");
        const auto blas = plan.makeBlasGeometry({17, 1}, {18, 1});
        ok &= check(blas.triangles.size() == 1 && blas.triangles.front().vertexCount == 130 &&
                        blas.triangles.front().indexCount == 192 &&
                        blas.triangles.front().vertexStride == sizeof(dayo::graphics::NativeDeformedVertex),
                    "native deform plan creates complete typed BLAS geometry");
        bool invalidThrew = false;
        try {
            static_cast<void>(dayo::graphics::makeNativeDeformPlan({.vertexCount = 3, .indexCount = 4}));
        } catch (const std::invalid_argument&) {
            invalidThrew = true;
        }
        ok &= check(invalidThrew, "native deform rejects non-triangle index counts");
    }

    // The native deform runtime owns uploadable source buffers and a device
    // local output buffer, then records the exact ordering required before
    // handing the output to BLAS build/refit.
    {
        const std::array<dayo::graphics::PreviewVertex, 2> vertices{};
        const std::array<dayo::graphics::PreviewBoneTransform, 1> bones{};
        const std::array<dayo::graphics::PreviewMorphDelta, 1> morphDeltas{};
        const std::array<float, 1> morphWeights{0.5F};
        const std::array<std::uint32_t, 3> indices{0, 1, 0};
        std::array<dayo::graphics::NativeDeformedVertex, 2> deformedVertices{};
        deformedVertices[0].position[0] = 3.0F;
        const dayo::graphics::NativeDeformUpload upload{
            .baseVertices = vertices,
            .bones = bones,
            .morphDeltas = morphDeltas,
            .morphWeights = morphWeights,
            .indices = indices,
            .deformedVertices = deformedVertices,
        };
        const auto layout = dayo::graphics::nativeDeformDescriptorLayout();
        ok &= check(layout.bindings.size() == 5 && layout.bindings.front().binding == 0 &&
                        layout.bindings.back().binding == 4,
                    "native deform descriptor ABI has five stable bindings");
        MockNativeDevice device;
        dayo::graphics::NativeDeformRuntime runtime;
        std::string error;
        ok &= check(runtime.initialize(device, upload, {10, 1}, {11, 1}, &error),
                    "native deform runtime allocates and uploads its resources");
        ok &= check(error.empty() && runtime.ready() && runtime.resources().valid(),
                    "native deform runtime exposes complete resource ownership");
        ok &= check(device.descriptorBindings_[runtime.resources().descriptorSet].size() == 5 &&
                        device.descriptorBindings_[runtime.resources().descriptorSet][4].buffer ==
                            runtime.resources().deformedVertices,
                    "native deform descriptor set binds the deformed output");
        const auto vertexBytes = vertices.size() * sizeof(vertices.front());
        const auto uploaded = device.readbackBufferEx(runtime.resources().baseVertices, 0, vertexBytes);
        ok &= check(uploaded.size() == vertexBytes, "native deform uploads base vertex data");
        const auto uploadedSeed = device.readbackBufferEx(runtime.resources().deformedVertices, 0,
                                                          deformedVertices.size() * sizeof(deformedVertices.front()));
        dayo::graphics::NativeDeformedVertex seed{};
        std::memcpy(&seed, uploadedSeed.data(), sizeof(seed));
        ok &= check(seed.position[0] == 3.0F, "native deform uploads the host seed for the initial BLAS");
        MockDeformCommands commands;
        runtime.record(commands);
        ok &= check(commands.events == std::vector<std::string>{"bind", "descriptor", "push", "dispatch:1x1x1"},
                    "native deform records bind descriptor constants and dispatch in order");
        ok &= check(commands.constants.size() == sizeof(dayo::graphics::NativeDeformPushConstants),
                    "native deform records its ABI-sized push constants");
        dayo::graphics::NativeDeformPushConstants constants{};
        std::memcpy(&constants, commands.constants.data(), commands.constants.size());
        ok &= check(constants.vertexCount == vertices.size() && constants.boneCount == bones.size() &&
                        constants.morphCount == morphWeights.size(),
                    "native deform push constants carry source counts");
        const auto baseVertexBuffer = runtime.resources().baseVertices;
        auto updatedVertices = vertices;
        updatedVertices[0].position[0] = 2.0F;
        auto updatedDeformedVertices = deformedVertices;
        updatedDeformedVertices[0].position[0] = 4.0F;
        const dayo::graphics::NativeDeformUpload updatedUpload{
            .baseVertices = updatedVertices,
            .bones = bones,
            .morphDeltas = morphDeltas,
            .morphWeights = morphWeights,
            .indices = indices,
            .deformedVertices = updatedDeformedVertices,
        };
        ok &= check(runtime.update(device, updatedUpload, &error) && error.empty(),
                    "native deform refreshes inputs without changing mesh resources");
        ok &= check(runtime.resources().baseVertices == baseVertexBuffer && device.destroyedBuffers == 0,
                    "native deform refresh keeps stable source buffer ownership");
        const auto refreshed = device.readbackBufferEx(runtime.resources().baseVertices, 0, vertexBytes);
        dayo::graphics::PreviewVertex refreshedVertex{};
        std::memcpy(&refreshedVertex, refreshed.data(), sizeof(refreshedVertex));
        ok &= check(refreshedVertex.position[0] == 2.0F,
                    "native deform refresh uploads the current animated vertex data");
        const auto refreshedSeed =
            device.readbackBufferEx(runtime.resources().deformedVertices, 0,
                                    updatedDeformedVertices.size() * sizeof(updatedDeformedVertices.front()));
        std::memcpy(&seed, refreshedSeed.data(), sizeof(seed));
        ok &= check(seed.position[0] == 4.0F, "native deform refresh uploads the current BLAS seed");
        MockDeformCommands topologyFrame0;
        runtime.record(topologyFrame0, updatedUpload, 1);
        ok &= check(topologyFrame0.events == std::vector<std::string>{"upload", "upload", "upload", "upload", "upload",
                                                                      "bind", "descriptor", "push", "dispatch:1x1x1"},
                    "native deform uploads static resources once for a new topology generation");
        device.frameSlot = 1;
        MockDeformCommands topologyFrame1;
        runtime.record(topologyFrame1, updatedUpload, 1);
        ok &= check(topologyFrame1.events == topologyFrame0.events,
                    "native deform initializes the inactive slot for the new topology generation");
        device.frameSlot = 0;
        MockDeformCommands topologyFrame2;
        runtime.record(topologyFrame2, updatedUpload, 1);
        ok &= check(topologyFrame2.events ==
                        std::vector<std::string>{"upload", "upload", "bind", "descriptor", "push", "dispatch:1x1x1"},
                    "native deform skips static scans and uploads only dynamic inputs on a current generation");
        const auto blas = runtime.blasGeometry();
        ok &= check(blas.triangles.front().vertexBuffer == runtime.resources().deformedVertices &&
                        blas.triangles.front().indexBuffer == runtime.resources().indices,
                    "native deform exposes output and index buffers for BLAS");
        runtime.reset();
        ok &= check(device.destroyedBuffers == 12 && device.destroyedDescriptorSets == 2,
                    "native deform reset releases both frame descriptor and buffer sets");
    }

    // Native geometry coordinates deform dispatches with BLAS/TLAS policy.
    // The persistent deformed output is synchronized only after the caller
    // has submitted the recorded compute work.
    {
        const std::array<dayo::graphics::PreviewVertex, 3> vertices{};
        const std::array<dayo::graphics::PreviewBoneTransform, 1> bones{};
        const std::array<dayo::graphics::PreviewMorphDelta, 1> morphDeltas{};
        const std::array<float, 1> morphWeights{0.25F};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        const dayo::graphics::NativeGeometryMeshUpload mesh{
            .meshId = 7,
            .deform = {.baseVertices = vertices,
                       .bones = bones,
                       .morphDeltas = morphDeltas,
                       .morphWeights = morphWeights,
                       .indices = indices,
                       .deformedVertices = {}},
            .deformPipeline = {10, 1},
            .deformDescriptorLayout = {11, 1},
            .topologyGeneration = 3,
            .deformVersion = 1,
        };
        MockNativeDevice device;
        MockAccelerationBackend backend;
        dayo::graphics::NativeGeometryRuntime runtime(&backend);
        std::string error;
        ok &= check(
            runtime.initialize(device, std::span<const dayo::graphics::NativeGeometryMeshUpload>(&mesh, 1), &error),
            "native geometry runtime initializes deform and acceleration state");
        ok &= check(runtime.descriptorLayout().valid(), "native geometry creates an acceleration descriptor layout");
        MockDeformCommands commands;
        backend.commandEvents = &commands.events;
        runtime.recordDeform(commands);
        ok &= check(commands.events ==
                        std::vector<std::string>{"bind", "descriptor", "push", "dispatch:1x1x1", "barrier"},
                    "native geometry records deform work before acceleration synchronization");
        ok &= check(runtime.synchronizeAcceleration(&error) && error.empty() && backend.createBlasCalls == 1,
                    "native geometry creates BLAS from the deformed output");
        ok &= check(runtime.blas(7).valid() && runtime.deform(7) != nullptr,
                    "native geometry exposes the mesh BLAS and deform runtime");
        const std::array<dayo::graphics::WorldInstance, 1> instances{{{.meshId = 7}}};
        ok &= check(runtime.synchronizeWorld(1, instances) == dayo::graphics::TlasAction::rebuild &&
                        runtime.tlas().valid(),
                    "native geometry creates a TLAS from registered world instances");
        ok &= check(runtime.descriptorSet().valid() && device.lastDescriptorBindings.size() == 1 &&
                        device.lastDescriptorBindings.front().accelerationStructure == runtime.tlas(),
                    "native geometry publishes the current TLAS through a typed descriptor set");
        runtime.recordAcceleration(commands);
        ok &= check(commands.events == std::vector<std::string>{"bind", "descriptor", "push", "dispatch:1x1x1",
                                                                "barrier", "as-barrier", "blas", "as-barrier", "tlas",
                                                                "as-barrier"} &&
                        backend.recordBlasCalls == 1 && backend.recordTlasCalls == 1,
                    "native geometry records BLAS then TLAS updates after deform work");

        auto updatedVertices = vertices;
        updatedVertices[0].position[0] = 1.0F;
        const dayo::graphics::NativeGeometryMeshUpload updatedMesh{
            .meshId = 7,
            .deform = {.baseVertices = updatedVertices,
                       .bones = bones,
                       .morphDeltas = morphDeltas,
                       .morphWeights = morphWeights,
                       .indices = indices,
                       .deformedVertices = {}},
            .deformPipeline = mesh.deformPipeline,
            .deformDescriptorLayout = mesh.deformDescriptorLayout,
            .topologyGeneration = 3,
            .deformVersion = 2,
        };
        ok &= check(runtime.updateMesh(updatedMesh, &error) && runtime.synchronizeAcceleration(&error) &&
                        backend.refitBlasCalls == 0,
                    "native geometry refreshes deform data without a synchronous BLAS refit");
        ok &= check(runtime.synchronizeWorld(2, instances) == dayo::graphics::TlasAction::update &&
                        backend.updateTlasCalls == 0,
                    "native geometry queues world-only TLAS transforms for the frame command list");
        runtime.recordDeform(commands, std::span<const dayo::graphics::NativeGeometryMeshUpload>(&updatedMesh, 1));
        runtime.recordAcceleration(commands);
        ok &= check(backend.recordBlasCalls == 2 && backend.recordTlasCalls == 2,
                    "native geometry records deferred BLAS and TLAS updates after deform work");
        runtime.reset();
        ok &= check(backend.destroyBlasCalls == 1 && backend.destroyTlasCalls == 1,
                    "native geometry reset releases the current frame AS resources");
        ok &= check(device.destroyedBuffers == 12, "native geometry reset releases both frame deform resource sets");
    }

    // BLAS branching: rebuild on topology, refit on deform-only, none otherwise.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        ok &= check(service.notifyMesh(0, geometry(11), 1, 1) == BlasAction::rebuild, "BLAS first build is rebuild");
        ok &= check(backend.createBlasCalls == 1, "BLAS create called once");
        ok &= check(service.notifyMesh(0, geometry(11), 1, 1) == BlasAction::none, "BLAS unchanged reports none");
        ok &= check(service.notifyMesh(0, geometry(11), 1, 2) == BlasAction::refit, "BLAS deform-only queues a refit");
        ok &= check(backend.refitBlasCalls == 0, "BLAS deform updates do not submit synchronously");
        MockDeformCommands commands;
        service.recordBlasUpdates(commands);
        ok &= check(backend.recordBlasCalls == 1 && backend.recordedVertexBuffers.size() == 1 &&
                        backend.recordedVertexBuffers.front().index == 11,
                    "recorded BLAS refit receives the current vertex buffer");
        ok &= check(service.notifyMesh(0, geometry(11), 2, 2) == BlasAction::rebuild, "BLAS topology change rebuilds");
        ok &= check(backend.rebuildBlasCalls == 1, "BLAS rebuild called once");
        ok &= check(backend.rebuildVertexBuffers.size() == 1 && backend.rebuildVertexBuffers.front().index == 11,
                    "BLAS rebuild receives the current vertex buffer");
        ok &= check(service.blasCount() == 1, "BLAS count tracks meshes");
    }
    // A changed vertex-buffer allocation is forwarded to BLAS rebuild/refit,
    // and service destruction releases all remaining backend resources.
    {
        MockAccelerationBackend backend;
        {
            AccelerationStructureService service(&backend);
            static_cast<void>(service.notifyMesh(0, geometry(11), 1, 1));
            static_cast<void>(service.notifyMesh(0, geometry(22), 1, 2));
            MockDeformCommands commands;
            service.recordBlasUpdates(commands);
            static_cast<void>(service.notifyMesh(0, geometry(33), 2, 3));
        }
        ok &= check(backend.refitBlasCalls == 0 && backend.recordedVertexBuffers.size() == 1 &&
                        backend.recordedVertexBuffers.front().index == 22,
                    "deform update forwards a replaced vertex buffer to recorded BLAS work");
        ok &= check(backend.rebuildVertexBuffers.size() == 1 && backend.rebuildVertexBuffers.front().index == 33,
                    "topology update forwards a replaced vertex buffer");
        ok &= check(backend.destroyBlasCalls == 1, "service destructor releases remaining BLAS");
    }
    // TLAS count reflects CloneCount; count/BLAS change rebuilds, world-only updates.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        static_cast<void>(service.notifyMesh(0, geometry(11), 1, 1));
        static_cast<void>(service.notifyMesh(1, geometry(12), 1, 1));
        const std::array<std::uint32_t, 2> clones{2, 3};
        ok &= check(service.notifyWorld(10, clones) == TlasAction::rebuild, "TLAS first build rebuilds");
        ok &= check(service.tlasInstanceCount() == 5, "TLAS instance count sums CloneCount");
        ok &= check(backend.lastTlasInstanceCount == 5, "TLAS backend receives every cloned instance");
        ok &= check(service.notifyWorld(10, clones) == TlasAction::none, "TLAS unchanged reports none");
        ok &= check(service.notifyWorld(11, clones) == TlasAction::update, "TLAS world change queues an update");
        ok &= check(backend.updateTlasCalls == 0, "TLAS world changes do not submit synchronously");
        MockDeformCommands commands;
        service.recordTlasUpdate(commands);
        ok &= check(backend.recordTlasCalls == 1, "TLAS update is recorded on the frame command list");
        const std::array<std::uint32_t, 2> grown{2, 4};
        ok &= check(service.notifyWorld(11, grown) == TlasAction::rebuild, "TLAS clone growth rebuilds");
        ok &= check(service.tlasInstanceCount() == 6, "TLAS instance count follows CloneCount");
        static_cast<void>(service.notifyMesh(0, geometry(11), 1, 9));
        ok &= check(service.notifyWorld(11, grown) == TlasAction::none, "BLAS refit keeps stable TLAS addresses");
        ok &= check(service.removeMesh(1), "removing a mesh succeeds");
        ok &= check(service.blasCount() == 1 && !service.tlasBuilt(), "mesh removal retires TLAS state");
        ok &= check(backend.destroyBlasCalls == 1 && backend.destroyTlasCalls == 1,
                    "mesh removal destroys BLAS and TLAS resources");
    }
    // A topology/count rebuild may replace the TLAS allocation. The service
    // must retire the old handle and continue tracking the replacement.
    {
        MockAccelerationBackend backend;
        backend.replaceTlasOnRebuild = true;
        AccelerationStructureService service(&backend);
        static_cast<void>(service.notifyMesh(0, geometry(11), 1, 1));
        const std::array<std::uint32_t, 1> initial{1};
        static_cast<void>(service.notifyWorld(1, initial));
        const std::array<std::uint32_t, 1> grown{2};
        ok &= check(service.notifyWorld(1, grown) == TlasAction::rebuild, "TLAS replacement rebuild reports rebuild");
        ok &= check(backend.rebuildTlasCalls == 1 && backend.destroyTlasCalls == 1,
                    "TLAS replacement retires the previous allocation");
    }
    // Transform-aware world instances preserve clone placement and metadata.
    {
        MockAccelerationBackend backend;
        AccelerationStructureService service(&backend);
        static_cast<void>(service.notifyMesh(10, geometry(11), 1, 1));
        static_cast<void>(service.notifyMesh(20, geometry(12), 1, 1));
        std::array<dayo::graphics::WorldInstance, 3> world{};
        world[0].meshId = 20;
        world[0].transform.values[3] = 4.0F;
        world[0].mask = 0x7FU;
        world[0].sbtRecordOffset = 3;
        world[1].meshId = 10;
        world[1].transform.values[7] = 5.0F;
        world[1].flags = 1;
        world[2].meshId = 20;
        world[2].transform.values[11] = 6.0F;
        ok &=
            check(service.notifyWorld(20, world) == TlasAction::rebuild, "transform-aware world instances build TLAS");
        ok &=
            check(backend.lastTlasInstances.size() == 3 &&
                      backend.lastTlasInstances[0].blas == dayo::graphics::handles::AccelerationStructureHandle{2, 1} &&
                      backend.lastTlasInstances[0].transform.values[3] == 4.0F &&
                      backend.lastTlasInstances[0].mask == 0x7FU && backend.lastTlasInstances[0].sbtRecordOffset == 3,
                  "TLAS preserves mesh mapping, transform, and metadata");
        ok &= check(backend.lastTlasInstances[1].blas == dayo::graphics::handles::AccelerationStructureHandle{1, 1} &&
                        backend.lastTlasInstances[1].transform.values[7] == 5.0F &&
                        backend.lastTlasInstances[1].flags == 1 && backend.lastTlasInstances[2].instanceId == 2,
                    "TLAS expands each world instance independently");
    }
    // EnvironmentService keeps cubemap/prefiltered/SH/Skywalker without regen.
    {
        MockEnvironmentBackend backend;
        EnvironmentService service(&backend);
        const EnvironmentDesc first{.source = "sky.hdr", .exposure = 1.0F, .version = 7};
        ok &= check(service.update(first), "environment first update regenerates");
        MockDeformCommands commands;
        service.record(commands);
        service.record(commands);
        ok &= check(backend.recordings == 1, "environment records pending GPU work only once");
        service.setHandles(11, 12, 7);
        ok &= check(!service.update(first), "environment unchanged reuses cache");
        ok &= check(backend.regenerations == 1 && service.generationCount() == 1, "environment regen counted once");
        ok &= check(service.cubemap() == 11 && service.prefilteredMips() == 12 && service.skywalkerVersion() == 7,
                    "environment keeps cubemap/prefiltered/Skywalker");
        ok &= check(service.sphericalHarmonics().size() == 27, "environment keeps SH coefficients");
        const EnvironmentDesc changed{.source = "sky.hdr", .exposure = 2.0F, .version = 7};
        ok &= check(service.update(changed), "environment exposure change regenerates");
        ok &= check(backend.regenerations == 2, "environment regen on change");
    }
    // Native environment regeneration owns the source/equirectangular image,
    // cubemap and prefiltered cubemap, while command recording performs the
    // two compute stages with an explicit inter-stage memory barrier.
    {
        MockNativeDevice device;
        const dayo::graphics::EnvironmentPassBindings bindings{
            .equirectToCubePipeline = {20, 1},
            .equirectToCubeLayout = {21, 1},
            .prefilterPipeline = {22, 1},
            .prefilterLayout = {23, 1},
        };
        const auto layout = dayo::graphics::nativeEnvironmentPassLayout();
        ok &= check(layout.bindings.size() == 2 &&
                        layout.bindings[0].kind == dayo::graphics::DescriptorKind::sampledImage &&
                        layout.bindings[1].kind == dayo::graphics::DescriptorKind::storageImage,
                    "native environment pass layout separates sampled input and storage output");
        const auto prefilterLayout = dayo::graphics::nativeEnvironmentPrefilterLayout();
        ok &= check(prefilterLayout.bindings.size() == 2 &&
                        prefilterLayout.bindings[0].kind == dayo::graphics::DescriptorKind::storageImage &&
                        prefilterLayout.bindings[1].kind == dayo::graphics::DescriptorKind::storageImage,
                    "native environment prefilter layout uses storage images");
        dayo::graphics::NativeEnvironmentBackend backend(device, bindings);
        const dayo::core::ImageData image{.width = 8,
                                          .height = 4,
                                          .channels = 4,
                                          .type = dayo::core::PixelType::unorm8,
                                          .space = dayo::core::ColorSpace::srgb,
                                          .bytes = std::vector<std::uint8_t>(128, 128)};
        const auto result = backend.regenerateImage({.source = "memory", .exposure = 1.0F, .version = 9}, image);
        ok &= check(backend.ready() && result.cubemap.valid() && result.prefiltered.valid() &&
                        result.skywalkerVersion == 9 && result.sphericalHarmonics[0] > 0.0F,
                    "native environment creates typed outputs and SH coefficients");
        MockDeformCommands commands;
        backend.record(commands);
        ok &= check(commands.events == std::vector<std::string>{"transition", "transition", "transition", "bind",
                                                                "descriptor", "push", "dispatch:1x1x6", "barrier",
                                                                "bind", "descriptor", "push", "dispatch:1x1x6",
                                                                "barrier", "mipmap"},
                    "native environment records conversion and prefilter stages with barriers");
        backend.reset();
        ok &= check(device.destroyedTextures == 3 && device.destroyedDescriptorSets == 2,
                    "native environment reset releases textures and descriptor sets");
    }
    // HDR environment sources are inspected before float decode and remain
    // linear ImageData for the environment conversion path.
    {
        const auto path = std::filesystem::temp_directory_path() / "dayo-native-environment-test.hdr";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n";
            for (int scanline = 0; scanline < 2; ++scanline) {
                output.put(static_cast<char>(2));
                output.put(static_cast<char>(2));
                output.put(static_cast<char>(0));
                output.put(static_cast<char>(4));
                for (int channel = 0; channel < 4; ++channel) {
                    output.put(static_cast<char>(132));
                    output.put(static_cast<char>(channel == 3 ? 129 : 128));
                }
            }
        }
        try {
            const auto image = dayo::core::loadImageData(path);
            ok &= check(image.width == 4 && image.height == 2 && image.channels == 4 &&
                            image.type == dayo::core::PixelType::float32 &&
                            image.space == dayo::core::ColorSpace::linear &&
                            image.byteSize() == 4U * 2U * 4U * sizeof(float),
                        "HDR environment loader returns bounded linear float data");
        } catch (const std::exception& exception) {
            std::cerr << "HDR test decode failed: " << exception.what() << '\n';
            ok = false;
        }
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
    }
    // LightSamplingService updates only on lighting dirty; light count from caller.
    {
        LightSamplingService service;
        const std::array<float, 3> powers{1.0F, 2.0F, 3.0F};
        service.update(powers, false);
        ok &= check(service.lightCount() == 0 && service.buildCount() == 0, "alias skips clean lighting");
        service.update(powers, true);
        ok &= check(service.lightCount() == 3 && service.buildCount() == 1, "alias builds from caller powers");
        const auto initialTable = service.table();
        double probabilitySum = 0.0;
        bool aliasesInRange = true;
        for (const AliasEntry& entry : initialTable) {
            probabilitySum += static_cast<double>(entry.probability);
            aliasesInRange &= entry.alias < initialTable.size();
            aliasesInRange &= entry.probability >= 0.0F && entry.probability <= 1.0F;
        }
        ok &= check(aliasesInRange, "alias entries reference valid lights");
        ok &= check(std::abs(probabilitySum - 1.5) < 2.0, "alias probabilities plausible");
        service.update(powers, false);
        ok &= check(service.buildCount() == 1, "alias keeps table without lighting dirty");
        const std::array<float, 5> moreLights{1.0F, 1.0F, 1.0F, 1.0F, 4.0F};
        service.update(moreLights, true);
        ok &= check(service.lightCount() == 5 && service.buildCount() == 2, "alias light count is caller-driven");

        const auto table = service.table();
        MockNativeDevice device;
        dayo::graphics::LightSamplingGpuRuntime gpu;
        std::string error;
        ok &= check(gpu.sync(device, table, &error) && gpu.ready() && gpu.count() == table.size(),
                    "light alias table uploads to a persistent typed buffer");
        const auto uploaded = device.readbackBufferEx(gpu.buffer(), 0, table.size() * sizeof(AliasEntry));
        ok &= check(uploaded.size() == table.size() * sizeof(AliasEntry),
                    "light alias GPU buffer contains the complete table");
        const auto slot0Buffer = gpu.buffer();
        auto changedTable = std::vector<AliasEntry>(table.begin(), table.end());
        changedTable[0].probability = 0.125F;
        device.frameSlot = 0;
        ok &= check(gpu.sync(device, changedTable, &error),
                    "light alias runtime uploads a changed generation to slot zero");
        device.frameSlot = 1;
        ok &= check(gpu.sync(device, changedTable, &error),
                    "light alias runtime uploads the same changed generation to slot one");
        const auto slot1Buffer = gpu.buffer();
        AliasEntry slot0Entry{};
        AliasEntry slot1Entry{};
        const auto slot0Bytes = device.readbackBufferEx(slot0Buffer, 0, sizeof(slot0Entry));
        const auto slot1Bytes = device.readbackBufferEx(slot1Buffer, 0, sizeof(slot1Entry));
        std::memcpy(&slot0Entry, slot0Bytes.data(), sizeof(slot0Entry));
        std::memcpy(&slot1Entry, slot1Bytes.data(), sizeof(slot1Entry));
        ok &= check(slot0Entry.probability == changedTable[0].probability &&
                        slot1Entry.probability == changedTable[0].probability &&
                        slot0Entry.alias == changedTable[0].alias && slot1Entry.alias == changedTable[0].alias,
                    "light alias slots contain the current generation");
        ok &= check(gpu.sync(device, changedTable, &error) && device.destroyedBuffers == 0,
                    "unchanged light alias generation reuses its buffer");
        ok &= check(gpu.sync(device, {}, &error) && !gpu.ready() && device.destroyedBuffers == 2,
                    "clearing lights releases both frame alias buffers");
    }
    // BDPT accumulation: dirty resets to 0+clear, otherwise increments.
    {
        BdptAccumulation accumulation;
        accumulation.ensurePersistent();
        ok &=
            check(accumulation.persistentReady() && accumulation.spectralLutReady() && accumulation.blackbodyLutReady(),
                  "BDPT spectral/blackbody LUTs ready");
        ok &= check(accumulation.volumeCount() == BdptAccumulation::kVolumeSlots, "BDPT keeps 8 volume slots");
        bool volumesValid = true;
        for (std::size_t index = 0; index < accumulation.volumeCount(); ++index) {
            volumesValid &= accumulation.volume(index).valid;
        }
        ok &= check(volumesValid, "BDPT volume slots valid");
        const auto* spectralBefore = accumulation.spectralLut().data();
        const auto* blackbodyBefore = accumulation.blackbodyLut().data();
        accumulation.ensurePersistent();
        ok &= check(accumulation.generationCount() == 1, "BDPT persistent resources created once");
        ok &= check(accumulation.spectralLut().data() == spectralBefore &&
                        accumulation.blackbodyLut().data() == blackbodyBefore,
                    "BDPT LUTs persist without regeneration");
        ok &= check(accumulation.beginFrame(dayo::core::DirtyFlag::geometry), "BDPT dirty requests clear");
        ok &= check(accumulation.sampleIndex() == 0 && accumulation.needsClear(), "BDPT dirty resets sample");
        ok &= check(!accumulation.beginFrame(dayo::core::DirtyFlag::none), "BDPT clean continues");
        ok &= check(accumulation.sampleIndex() == 1 && !accumulation.needsClear(), "BDPT clean increments");
        ok &= check(!accumulation.beginFrame(dayo::core::DirtyFlag::none), "BDPT second clean continues");
        ok &= check(accumulation.sampleIndex() == 2, "BDPT sample index accumulates");
        dayo::core::Scene scene;
        BdptAccumulation synced;
        ok &= check(synced.syncScene(scene), "BDPT scene initial dirty resets");
        ok &= check(synced.sampleIndex() == 0 && scene.accumulatedSamples() == 0, "BDPT scene reset clears");
        scene.clearDirty();
        ok &= check(!synced.syncScene(scene), "BDPT scene clean accumulates");
        ok &= check(synced.sampleIndex() == 1 && scene.accumulatedSamples() == 1, "BDPT scene uses accumulatedSamples");
    }
    // ShaderBindingTableBuilder: shared raygen/miss/hit layout.
    {
        ShaderBindingTableBuilder builder;
        builder.setRaygen("raygen");
        builder.addMiss("miss0");
        builder.addMiss("miss1");
        builder.addHitGroup("hit0");
        builder.addHitGroup("hit1");
        builder.addHitGroup("hit2");
        builder.addCallable("callable0");
        ok &= check(builder.totalGroups() == 7, "SBT tracks raygen/miss/hit/callable groups");
        const auto layout = builder.build(0x1000, 32, 32);
        ok &= check(layout.raygenAddress == 0x1000, "SBT raygen base address");
        ok &= check(layout.missAddress == 0x1000 + 32, "SBT miss follows raygen");
        ok &= check(layout.hitAddress == 0x1000 + 32 + 2 * 32, "SBT hit follows miss");
        ok &= check(layout.totalSize == 7 * 32, "SBT total size covers shared groups");
        ok &= check(layout.raygenStride == 32 && layout.missStride == 32 && layout.hitStride == 32,
                    "SBT shares stride across groups");
        const auto aligned = builder.build(0x2000, 20, 32);
        ok &= check(aligned.raygenStride == 32 && aligned.totalSize == 7 * 32, "SBT aligns handles");
        const ShaderBindingTableBuilder::Properties properties{20, 32, 64, 64};
        const auto vulkanValid = builder.build(0x2001, properties);
        ok &= check(vulkanValid.raygenAddress % 64 == 0 && vulkanValid.missAddress % 64 == 0 &&
                        vulkanValid.hitAddress % 64 == 0,
                    "SBT aligns every region base to shaderGroupBaseAlignment");
        ok &= check(vulkanValid.raygenStride == 32 && vulkanValid.totalSize >= 6 * 32,
                    "SBT properties enforce aligned stride and padded allocation");
        ok &= check(builder.build(0x2000, ShaderBindingTableBuilder::Properties{65, 32, 64, 64}).totalSize == 0,
                    "SBT rejects maxShaderGroupStride overflow");
    }
    // Native scene descriptor ABI: fixed spaces and disjoint HLSL register
    // class bindings match the upstream resources.hlsli contract.
    {
        const dayo::graphics::NativeSceneDescriptorCounts counts{
            .textures = 7,
            .vertexBuffers = 3,
            .indexBuffers = 4,
            .materials = 5,
            .faces = 6,
            .materialFaces = 7,
            .faceWalkers = 8,
            .previousVertices = 9,
            .rawVertices = 10,
        };
        const auto layouts = dayo::graphics::nativeSceneDescriptorLayouts(counts);
        const auto binding = [](const dayo::graphics::DescriptorSetLayoutDesc& layout,
                                std::uint32_t slot) -> const dayo::graphics::DescriptorSetLayoutBinding* {
            const auto found = std::find_if(layout.bindings.begin(), layout.bindings.end(),
                                            [slot](const auto& value) { return value.binding == slot; });
            return found == layout.bindings.end() ? nullptr : &*found;
        };
        const auto frameIndex = static_cast<std::size_t>(dayo::graphics::NativeSceneDescriptorSet::frame);
        const auto texturesIndex = static_cast<std::size_t>(dayo::graphics::NativeSceneDescriptorSet::textures);
        const auto rawVerticesIndex = static_cast<std::size_t>(dayo::graphics::NativeSceneDescriptorSet::rawVertices);
        ok &= check(layouts.size() == dayo::graphics::kNativeSceneDescriptorSetCount,
                    "native scene reserves ten descriptor spaces");
        ok &= check(dayo::graphics::nativeFxResourceSet() == dayo::graphics::kNativeSceneDescriptorSetCount,
                    "FX-local resources follow native scene descriptor spaces");
        ok &=
            check(dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uav, 0) == 0 &&
                      dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::sampled, 0) == 16 &&
                      dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::sampler, 0) == 32 &&
                      dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uniform, 0) == 48,
                  "native scene register classes use disjoint Vulkan binding ranges");
        const auto* rtOutput = binding(layouts[frameIndex], 0);
        const auto* viewConstants = binding(layouts[frameIndex], 48);
        const auto* controllerConstants = binding(layouts[frameIndex], 49);
        const auto* tlas = binding(layouts[frameIndex], 16);
        const auto* screenTexture = binding(layouts[frameIndex], 27);
        ok &= check(rtOutput != nullptr && rtOutput->kind == dayo::graphics::DescriptorKind::storageImage,
                    "native frame binds RTOutput as a storage image");
        ok &= check(viewConstants != nullptr && viewConstants->kind == dayo::graphics::DescriptorKind::uniformBuffer &&
                        controllerConstants != nullptr &&
                        controllerConstants->kind == dayo::graphics::DescriptorKind::uniformBuffer,
                    "native frame reserves ViewCB and generated controller constants");
        ok &= check(tlas != nullptr && tlas->kind == dayo::graphics::DescriptorKind::accelerationStructure,
                    "native frame binds TLAS as an acceleration structure");
        ok &= check(screenTexture != nullptr && screenTexture->kind == dayo::graphics::DescriptorKind::sampledImage,
                    "native frame reserves ScreenTexture at t11");
        const auto* textureTable = binding(layouts[texturesIndex], 16);
        const auto* textures = binding(layouts[texturesIndex], 17);
        const auto* frameConstants = binding(layouts[texturesIndex], 48);
        ok &= check(textureTable != nullptr && textureTable->kind == dayo::graphics::DescriptorKind::storageBuffer &&
                        textures != nullptr && textures->count == counts.textures &&
                        textures->kind == dayo::graphics::DescriptorKind::sampledImage && frameConstants != nullptr &&
                        frameConstants->kind == dayo::graphics::DescriptorKind::uniformBuffer,
                    "native texture space preserves table, texture array, and CBuff1");
        const auto* rawVertices = binding(layouts[rawVerticesIndex], 16);
        ok &= check(rawVertices != nullptr && rawVertices->count == counts.rawVertices &&
                        rawVertices->kind == dayo::graphics::DescriptorKind::storageBuffer,
                    "native raw vertex space preserves runtime array count");
        const auto emptyCounts =
            dayo::graphics::nativeSceneDescriptorLayouts(dayo::graphics::NativeSceneDescriptorCounts{.textures = 0});
        const auto* emptyTextures = binding(emptyCounts[texturesIndex], 17);
        ok &= check(emptyTextures != nullptr && emptyTextures->count == 1,
                    "empty native arrays retain a legal placeholder descriptor");
    }
    // NativeSceneBindingRuntime allocates only complete fixed-space sets.
    {
        MockNativeDevice device;
        dayo::graphics::NativeSceneBindingRuntime runtime;
        const dayo::graphics::NativeSceneDescriptorCounts counts{.textures = 2};
        std::string error;
        ok &= check(runtime.initialize(device, counts, &error) && runtime.ready(),
                    "native scene binding runtime creates all reserved layouts");
        ok &= check(runtime.layouts().size() == dayo::graphics::kNativeSceneDescriptorSetCount &&
                        runtime.layout(dayo::graphics::NativeSceneDescriptorSet::textures).valid(),
                    "native scene binding runtime exposes indexed layouts");
        const std::array<dayo::graphics::DescriptorBindingEx, 4> incomplete{
            dayo::graphics::DescriptorBindingEx{.slot = 16, .arrayElement = 0, .buffer = {1, 1}},
            dayo::graphics::DescriptorBindingEx{.slot = 17, .arrayElement = 0, .texture = {2, 1}},
            dayo::graphics::DescriptorBindingEx{.slot = 17, .arrayElement = 1, .texture = {3, 1}},
            dayo::graphics::DescriptorBindingEx{.slot = 48, .arrayElement = 0, .buffer = {4, 1}},
        };
        ok &= check(runtime.bind(dayo::graphics::NativeSceneDescriptorSet::textures, incomplete, &error) &&
                        runtime.descriptorSet(dayo::graphics::NativeSceneDescriptorSet::textures).valid(),
                    "native scene binding runtime binds complete array sets");
        const std::array<dayo::graphics::DescriptorBindingEx, 3> partial{
            dayo::graphics::DescriptorBindingEx{.slot = 16, .arrayElement = 0, .buffer = {1, 1}},
            dayo::graphics::DescriptorBindingEx{.slot = 17, .arrayElement = 0, .texture = {2, 1}},
            dayo::graphics::DescriptorBindingEx{.slot = 48, .arrayElement = 0, .buffer = {4, 1}},
        };
        ok &=
            check(!runtime.bind(dayo::graphics::NativeSceneDescriptorSet::textures, partial, &error) && !error.empty(),
                  "native scene binding runtime rejects partial arrays before allocation");
        const auto destroyedLayouts = device.destroyedDescriptorLayouts;
        runtime.reset();
        ok &= check(device.destroyedDescriptorLayouts ==
                        destroyedLayouts + dayo::graphics::kNativeSceneDescriptorSetCount,
                    "native scene binding runtime releases reserved layouts");
    }
    // Native frame constants: CPU ABI and typed uniform uploads remain stable
    // independently of the native scene descriptor-set population.
    {
        const std::array<dayo::core::EffectController, 6> controllers{{
            {.name = "Exposure", .controllerName = {}, .item = {}, .type = "float"},
            {.name = "Tint", .controllerName = {}, .item = {}, .type = "float3"},
            {.name = "Enabled", .controllerName = {}, .item = {}, .type = "bool"},
            {.name = "Transform", .controllerName = {}, .item = {}, .type = "float4x4"},
            {.name = "Samples[2]", .controllerName = {}, .item = {}, .type = "float"},
            {.name = "Mode", .controllerName = {}, .item = {}, .type = "int"},
        }};
        const auto layout = dayo::graphics::makeNativeControllerLayout(controllers);
        const auto* exposure = layout.find("Exposure");
        const auto* tint = layout.find("Tint");
        const auto* enabled = layout.find("Enabled");
        const auto* transform = layout.find("Transform");
        const auto* samples = layout.find("Samples");
        const auto* mode = layout.find("Mode");
        ok &=
            check(exposure != nullptr && exposure->offset == 0 && tint != nullptr && tint->offset == 4 &&
                      enabled != nullptr && enabled->offset == 16 && transform != nullptr && transform->offset == 32 &&
                      samples != nullptr && samples->offset == 96 && samples->elementStride == 16 && mode != nullptr &&
                      mode->offset == 128 && layout.byteSize == 144,
                  "native controller layout follows HLSL register packing");
        dayo::graphics::NativeControllerBlock block(layout);
        const std::array<float, 3> tintValue{0.1F, 0.2F, 0.3F};
        const std::array<float, 16> transformValue{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                   0.0F, 0.0F, 1.0F, 0.0F, 4.0F, 5.0F, 6.0F, 1.0F};
        ok &= check(block.setFloat("Exposure", 1.25F) && block.setFloat3("Tint", tintValue) &&
                        block.setBool("Enabled", true) && block.setMatrix4x4("Transform", transformValue) &&
                        block.setFloat("Samples", 2.0F, 1) && block.setInt("Mode", 7) && !block.setFloat("Mode", 1.0F),
                    "native controller block writes typed scalar, vector, matrix, and array values");
        MockNativeDevice device;
        dayo::graphics::NativeControllerRuntime runtime;
        std::string error;
        ok &= check(runtime.initialize(device, controllers, &error) && runtime.ready() &&
                        runtime.layout().byteSize == layout.byteSize && runtime.sync(device, block.bytes(), &error),
                    "native controller runtime uploads the generated cbuffer");
        const auto uploaded = device.readbackBufferEx(runtime.buffer(), 0, layout.byteSize);
        float exposureValue{};
        float sampleValue{};
        std::int32_t enabledValue{};
        std::memcpy(&exposureValue, uploaded.data() + exposure->offset, sizeof(exposureValue));
        std::memcpy(&sampleValue, uploaded.data() + samples->offset + samples->elementStride, sizeof(sampleValue));
        std::memcpy(&enabledValue, uploaded.data() + enabled->offset, sizeof(enabledValue));
        ok &=
            check(std::abs(exposureValue - 1.25F) < 1e-6F && std::abs(sampleValue - 2.0F) < 1e-6F && enabledValue == 1,
                  "native controller upload preserves packed values");
        ok &= check(!runtime.sync(device, std::span<const std::byte>(uploaded.data(), uploaded.size() - 1), &error) &&
                        !error.empty(),
                    "native controller runtime rejects a mismatched cbuffer size");
    }
    // Native scene frame runtime keeps controller, frame, and descriptor-set
    // state synchronized at one frame boundary.
    {
        MockNativeDevice device;
        const std::array<dayo::core::EffectController, 1> controllers{{
            {.name = "Gain", .controllerName = {}, .item = {}, .type = "float"},
        }};
        dayo::graphics::NativeSceneFrameRuntime runtime;
        std::string error;
        ok &= check(runtime.initialize(device, controllers, {}, &error) && runtime.ready() &&
                        runtime.layouts().size() == dayo::graphics::kNativeSceneDescriptorSetCount,
                    "native scene frame runtime initializes canonical layouts and constants");
        ok &= check(runtime.controllerBlock() != nullptr && runtime.controllerBlock()->setFloat("Gain", 3.5F),
                    "native scene frame runtime exposes typed controller values");

        std::uint32_t nextHandle = 1;
        const auto buffer = [&nextHandle]() { return dayo::graphics::handles::BufferHandle{nextHandle++, 1}; };
        const auto texture = [&nextHandle]() { return dayo::graphics::handles::TextureHandle{nextHandle++, 1}; };
        const auto acceleration = [&nextHandle]() {
            return dayo::graphics::handles::AccelerationStructureHandle{nextHandle++, 1};
        };
        const auto textureValue = texture();
        const auto bufferValue = buffer();
        const std::array<dayo::graphics::handles::TextureHandle, 1> textures{textureValue};
        const std::array<dayo::graphics::handles::BufferHandle, 1> buffers{bufferValue};
        dayo::graphics::NativeSceneResourceBindings resources;
        resources.rtOutput = texture();
        resources.oidnBuffer = buffer();
        resources.normalDepth = texture();
        resources.gbuffer1 = texture();
        resources.gbuffer2 = texture();
        resources.tlas = acceleration();
        resources.modelToMaterial = buffer();
        resources.materialToModel = buffer();
        resources.peekaboo = buffer();
        resources.materialSelected = buffer();
        resources.skybox = texture();
        resources.skywalker = buffer();
        resources.skywalkerRow = buffer();
        resources.skyboxSh = buffer();
        resources.screenBmp = texture();
        resources.cloneCount = buffer();
        resources.screenTexture = texture();
        resources.textureTable = buffer();
        resources.textures = textures;
        resources.vertexBuffers = buffers;
        resources.indexBuffers = buffers;
        resources.materials = buffers;
        resources.faces = buffers;
        resources.materialFaces = buffers;
        resources.faceWalkers = buffers;
        resources.previousVertices = buffers;
        resources.rawVertices = buffers;
        const auto context = dayo::fx::makeFxFrameContext(12.0F, 4, 320, 200, 1, 0, 8, 2, 1, 1);
        const dayo::graphics::NativeScenePassConstants pass{
            .modelIndex = 1, .rasterizeOrder = 2, .deformIndex = 3, .deformOrder = 4};
        ok &= check(runtime.sync(context, resources, pass, &error),
                    "native scene frame runtime synchronizes constants and canonical resources");
        ok &= check(runtime.descriptorSetsReady(),
                    "native scene frame runtime exposes a complete canonical descriptor set span");
        const auto controllerField = runtime.controllerBlock()->layout().find("Gain");
        float gain{};
        if (controllerField != nullptr) {
            const auto controllerBytes = device.readbackBufferEx(runtime.controllers().buffer(), 0,
                                                                 runtime.controllerBlock()->layout().byteSize);
            std::memcpy(&gain, controllerBytes.data() + controllerField->offset, sizeof(gain));
        }
        const auto viewBytes =
            device.readbackBufferEx(runtime.constants().viewBuffer(), 0, sizeof(dayo::graphics::NativeViewConstants));
        dayo::graphics::NativeViewConstants view{};
        std::memcpy(&view, viewBytes.data(), sizeof(view));
        ok &= check(std::abs(gain - 3.5F) < 1e-6F && view.output[0] == 320 && view.output[1] == 200 &&
                        view.output[2] == 4,
                    "native scene frame runtime uploads controller and frame values together");
        auto sampleOnly = context;
        sampleOnly.sample = 9;
        ok &= check(runtime.syncViewConstants(sampleOnly, &error),
                    "native scene frame runtime refreshes ViewCB without rebinding scene resources");
        const auto refreshedViewBytes =
            device.readbackBufferEx(runtime.constants().viewBuffer(), 0, sizeof(dayo::graphics::NativeViewConstants));
        std::memcpy(&view, refreshedViewBytes.data(), sizeof(view));
        ok &= check(view.output[2] == 9, "native scene frame runtime publishes a changed sample index");
    }
    // Canonical scene CPU buffers preserve the upstream StructuredBuffer
    // layout independently from Preview and the compact BLAS vertex format.
    {
        mmd::PmxModel model;
        model.vertices.resize(4);
        model.vertices[0].position = {0.0F, 0.0F, 0.0F};
        model.vertices[1].position = {1.0F, 0.0F, 0.0F};
        model.vertices[2].position = {1.0F, 1.0F, 0.0F};
        model.vertices[3].position = {0.0F, 1.0F, 0.0F};
        for (auto& vertex : model.vertices) {
            vertex.normal = {0.0F, 0.0F, 1.0F};
            vertex.uv = {vertex.position[0], vertex.position[1]};
        }
        model.indices = {0, 1, 2, 0, 2, 3};
        model.materials.resize(2);
        model.materials[0].diffuse = {1.0F, 0.0F, 0.0F, 1.0F};
        model.materials[0].indexCount = 3;
        model.materials[1].diffuse = {0.0F, 1.0F, 0.0F, 1.0F};
        model.materials[1].indexCount = 3;
        std::array<dayo::graphics::PreviewVertex, 4> preview{};
        for (std::size_t index = 0; index < preview.size(); ++index) {
            std::copy(model.vertices[index].position.begin(), model.vertices[index].position.end(),
                      std::begin(preview[index].position));
            std::copy(model.vertices[index].normal.begin(), model.vertices[index].normal.end(),
                      std::begin(preview[index].normal));
            std::copy(model.vertices[index].uv.begin(), model.vertices[index].uv.end(), std::begin(preview[index].uv));
        }
        const auto data = dayo::graphics::makeNativeSceneModelData(model, preview);
        ok &= check(data.vertices.size() == 4 && data.indices == model.indices && data.materials.size() == 2 &&
                        data.faces == std::vector<std::uint32_t>{0, 1} && data.materialFaces.size() == 2 &&
                        data.faceWalker.size() == 2,
                    "native scene data expands PMX model buffers deterministically");
        ok &= check(std::abs(data.vertices[0].tangent[0] - 1.0F) < 1e-5F &&
                        std::abs(data.vertices[0].tangent[1]) < 1e-5F &&
                        std::abs(data.materialFaces[0].totalArea - 0.5F) < 1e-5F &&
                        std::abs(data.materialFaces[1].totalArea - 0.5F) < 1e-5F,
                    "native scene data reconstructs tangent and material face area");
        ok &= check(data.materials[0].vertexCount == 3 && data.materials[1].vertexCount == 3,
                    "native scene data preserves material index counts");

        MockNativeDevice device;
        dayo::graphics::NativeSceneModelRuntime runtime;
        const std::array<dayo::graphics::NativeSceneModelData, 1> models{data};
        std::string error;
        ok &= check(runtime.sync(device, models, &error) && runtime.ready() && runtime.modelCount() == 1,
                    "native scene model runtime allocates one buffer set per model");
        const auto counts = runtime.descriptorCounts();
        const auto bindings = runtime.bindings();
        const auto vertexUsage = std::ranges::find_if(device.typedBufferUsages, [&](const auto& entry) {
            return entry.first == bindings.vertexBuffers.front();
        });
        const auto requiredVertexUsage = dayo::graphics::ResourceUsage::storageReadWrite |
                                         dayo::graphics::ResourceUsage::vertexRead |
                                         dayo::graphics::ResourceUsage::asBuildRead |
                                         dayo::graphics::ResourceUsage::rayTracingRead;
        ok &= check(counts.vertexBuffers == 1 && counts.indexBuffers == 1 && counts.materials == 1 &&
                        bindings.vertexBuffers.size() == 1 && bindings.indexBuffers.size() == 1 &&
                        bindings.materials.size() == 1 && bindings.previousVertices.size() == 1 &&
                        bindings.rawVertices.size() == 1 && bindings.previousVertices[0] != bindings.vertexBuffers[0] &&
                        bindings.rawVertices[0] != bindings.vertexBuffers[0] &&
                        vertexUsage != device.typedBufferUsages.end() &&
                        (dayo::graphics::toBits(vertexUsage->second) & dayo::graphics::toBits(requiredVertexUsage)) ==
                            dayo::graphics::toBits(requiredVertexUsage),
                    "native scene model runtime exposes writable final vertices to raster and ray tracing");
        const auto uploaded =
            device.readbackBufferEx(bindings.vertexBuffers[0], 0, sizeof(dayo::graphics::NativeSceneVertex));
        dayo::graphics::NativeSceneVertex firstVertex{};
        std::memcpy(&firstVertex, uploaded.data(), sizeof(firstVertex));
        ok &= check(std::abs(firstVertex.position[0] - data.vertices[0].position[0]) < 1e-6F &&
                        std::abs(firstVertex.tangent[0] - data.vertices[0].tangent[0]) < 1e-6F,
                    "native scene model runtime uploads the canonical vertex ABI");
        auto updatedModels = models;
        updatedModels[0].vertices[0].position[0] = 2.0F;
        const auto vertexBuffer = bindings.vertexBuffers[0];
        const auto previousBuffer = bindings.previousVertices[0];
        const auto rawBuffer = bindings.rawVertices[0];
        ok &=
            check(runtime.update(device, updatedModels, &error) && runtime.bindings().vertexBuffers[0] == vertexBuffer,
                  "native scene model runtime reuses buffers when the ABI layout is unchanged");
        const auto updated = device.readbackBufferEx(vertexBuffer, 0, sizeof(dayo::graphics::NativeSceneVertex));
        std::memcpy(&firstVertex, updated.data(), sizeof(firstVertex));
        ok &= check(std::abs(firstVertex.position[0] - 2.0F) < 1e-6F,
                    "native scene model runtime refreshes animated canonical vertices in place");
        const auto previous = device.readbackBufferEx(previousBuffer, 0, sizeof(dayo::graphics::NativeSceneVertex));
        std::memcpy(&firstVertex, previous.data(), sizeof(firstVertex));
        ok &= check(std::abs(firstVertex.position[0] - data.vertices[0].position[0]) < 1e-6F,
                    "native scene model runtime preserves the preceding vertex stream");
        const auto raw = device.readbackBufferEx(rawBuffer, 0, sizeof(dayo::graphics::NativeSceneVertex));
        std::memcpy(&firstVertex, raw.data(), sizeof(firstVertex));
        ok &= check(std::abs(firstVertex.position[0] - data.vertices[0].position[0]) < 1e-6F,
                    "native scene model runtime keeps the raw vertex stream immutable");

        updatedModels[0].vertices[0].position[0] = 3.0F;
        MockNativeSceneCommands frameCommands(device);
        const auto uploadsBeforeFrame = device.uploadBufferCalls;
        ok &= check(runtime.updateFrame(device, frameCommands, updatedModels, &error),
                    "native scene model runtime records animated transfers in the frame command list");
        ok &= check(frameCommands.copies.size() == 2 &&
                        frameCommands.events ==
                            std::vector<std::string>{"memory-barrier", "copy", "copy", "memory-barrier"} &&
                        !frameCommands.transferBarrierRecorded && frameCommands.barrierRecorded &&
                        device.uploadBufferCalls == uploadsBeforeFrame + 1,
                    "native scene frame update orders history/current copies with cross-frame barriers");
        const auto frameCurrent = device.readbackBufferEx(vertexBuffer, 0, sizeof(dayo::graphics::NativeSceneVertex));
        std::memcpy(&firstVertex, frameCurrent.data(), sizeof(firstVertex));
        ok &= check(std::abs(firstVertex.position[0] - 3.0F) < 1e-6F,
                    "native scene frame update publishes the current vertex stream");
        const auto framePrevious =
            device.readbackBufferEx(previousBuffer, 0, sizeof(dayo::graphics::NativeSceneVertex));
        std::memcpy(&firstVertex, framePrevious.data(), sizeof(firstVertex));
        ok &= check(std::abs(firstVertex.position[0] - 2.0F) < 1e-6F,
                    "native scene frame update preserves the prior current vertex stream");

        updatedModels[0].materials[0].diffuse[0] = 0.25F;
        ++updatedModels[0].materialGeneration;
        const auto copiesBeforeMaterial = frameCommands.copies.size();
        ok &= check(runtime.updateFrame(device, frameCommands, updatedModels, &error),
                    "native scene frame update accepts a changed material buffer");
        ok &= check(frameCommands.copies.size() == copiesBeforeMaterial + 3,
                    "native scene frame update records only the changed material and vertex transfers");

        // Temporal vertex history must follow the preceding logical frame,
        // not the buffer that happens to share the current frame slot.
        MockNativeDevice historyDevice;
        dayo::graphics::NativeSceneModelRuntime historyRuntime;
        ok &= check(historyRuntime.sync(historyDevice, models, &error),
                    "native scene history runtime initializes both frame slots");
        auto historyModels = models;
        const auto previousPosition = [&]() {
            dayo::graphics::NativeSceneVertex vertex{};
            const auto bytes =
                historyDevice.readbackBufferEx(historyRuntime.bindings().previousVertices[0], 0, sizeof(vertex));
            std::memcpy(&vertex, bytes.data(), sizeof(vertex));
            return vertex.position[0];
        };
        historyDevice.frameSlot = 0;
        historyModels[0].vertices[0].position[0] = 1.0F;
        MockNativeSceneCommands historyFrame0(historyDevice);
        ok &= check(historyRuntime.updateFrame(historyDevice, historyFrame0, historyModels, &error),
                    "native scene history records frame zero");
        ok &= check(std::abs(previousPosition() - 0.0F) < 1e-6F,
                    "native scene history frame zero exposes the initial vertex stream");

        historyDevice.frameSlot = 1;
        historyModels[0].vertices[0].position[0] = 2.0F;
        MockNativeSceneCommands historyFrame1(historyDevice);
        ok &= check(historyRuntime.updateFrame(historyDevice, historyFrame1, historyModels, &error),
                    "native scene history records frame one");
        ok &= check(std::abs(previousPosition() - 1.0F) < 1e-6F,
                    "native scene history frame one exposes frame zero vertices");

        historyDevice.frameSlot = 0;
        historyModels[0].vertices[0].position[0] = 3.0F;
        MockNativeSceneCommands historyFrame2(historyDevice);
        ok &= check(historyRuntime.updateFrame(historyDevice, historyFrame2, historyModels, &error),
                    "native scene history records frame two");
        ok &= check(std::abs(previousPosition() - 2.0F) < 1e-6F,
                    "native scene history frame two exposes frame one vertices");
    }
    // Native frame constants: CPU ABI and typed uniform uploads remain stable
    // independently of the native scene descriptor-set population.
    {
        MockNativeDevice device;
        auto context = dayo::fx::makeFxFrameContext(30.0F, 7, 640, 360, 11, 2, 12, 4, 1, 1);
        context.modelCount = 3;
        const auto view = dayo::graphics::makeNativeViewConstants(context, 4);
        const dayo::graphics::NativeScenePassConstants pass{
            .modelIndex = 2, .rasterizeOrder = 1, .deformIndex = 2, .deformOrder = 0};
        dayo::graphics::NativeFrameConstantsRuntime runtime;
        std::string error;
        ok &= check(runtime.sync(device, view, pass, &error) && runtime.ready(),
                    "native frame constants upload creates typed uniform buffers");
        const auto viewBytes = device.readbackBufferEx(runtime.viewBuffer(), 0, sizeof(view));
        const auto passBytes = device.readbackBufferEx(runtime.passBuffer(), 0, sizeof(pass));
        dayo::graphics::NativeViewConstants uploadedView{};
        dayo::graphics::NativeScenePassConstants uploadedPass{};
        std::memcpy(&uploadedView, viewBytes.data(), sizeof(uploadedView));
        std::memcpy(&uploadedPass, passBytes.data(), sizeof(uploadedPass));
        ok &= check(uploadedView.output == std::array<std::uint32_t, 4>{640, 360, 7, 1} &&
                        uploadedView.modelCounts == std::array<std::uint32_t, 2>{3, 4} &&
                        uploadedView.cameraFlags[0] == (context.camera.perspective ? 1 : 0),
                    "native ViewCB preserves frame dimensions, sample, and scene counts");
        ok &= check(uploadedPass.modelIndex == pass.modelIndex && uploadedPass.deformIndex == pass.deformIndex,
                    "native CBuff1 preserves model and deform selection");
        const auto destroyedBeforeReset = device.destroyedBuffers;
        runtime.reset();
        ok &= check(device.destroyedBuffers == destroyedBeforeReset + 4,
                    "native frame constants reset releases both frame-indexed uniform-buffer pairs");
    }
    // Native scene resources: fixed frame bindings and all runtime arrays are
    // materialized into the upstream descriptor spaces in one operation.
    {
        MockNativeDevice device;
        dayo::graphics::NativeSceneResourceRuntime runtime;
        const dayo::graphics::NativeSceneDescriptorCounts counts{
            .textures = 2,
            .vertexBuffers = 1,
            .indexBuffers = 1,
            .materials = 1,
            .faces = 1,
            .materialFaces = 1,
            .faceWalkers = 1,
            .previousVertices = 1,
            .rawVertices = 1,
        };
        std::string error;
        ok &= check(runtime.initialize(device, counts, &error),
                    "native scene resource runtime creates canonical layouts");
        std::uint32_t nextHandle = 1;
        const auto buffer = [&nextHandle]() { return dayo::graphics::handles::BufferHandle{nextHandle++, 1}; };
        const auto texture = [&nextHandle]() { return dayo::graphics::handles::TextureHandle{nextHandle++, 1}; };
        const auto acceleration = [&nextHandle]() {
            return dayo::graphics::handles::AccelerationStructureHandle{nextHandle++, 1};
        };
        const std::array<dayo::graphics::handles::TextureHandle, 2> textures{texture(), texture()};
        const std::array<dayo::graphics::handles::BufferHandle, 1> buffers{buffer()};
        dayo::graphics::NativeSceneResourceBindings resources;
        resources.rtOutput = texture();
        resources.oidnBuffer = buffer();
        resources.normalDepth = texture();
        resources.gbuffer1 = texture();
        resources.gbuffer2 = texture();
        resources.tlas = acceleration();
        resources.modelToMaterial = buffer();
        resources.materialToModel = buffer();
        resources.peekaboo = buffer();
        resources.materialSelected = buffer();
        resources.skybox = texture();
        resources.skywalker = buffer();
        resources.skywalkerRow = buffer();
        resources.skyboxSh = buffer();
        resources.screenBmp = texture();
        resources.cloneCount = buffer();
        resources.screenTexture = texture();
        resources.viewConstants = buffer();
        resources.controllerConstants = buffer();
        resources.textureTable = buffer();
        resources.textures = textures;
        resources.passConstants = buffer();
        resources.vertexBuffers = buffers;
        resources.indexBuffers = buffers;
        resources.materials = buffers;
        resources.faces = buffers;
        resources.materialFaces = buffers;
        resources.faceWalkers = buffers;
        resources.previousVertices = buffers;
        resources.rawVertices = buffers;
        ok &= check(runtime.sync(resources, &error),
                    "native scene resource runtime binds every canonical descriptor space");
        ok &= check(runtime.descriptorSet(dayo::graphics::NativeSceneDescriptorSet::frame).valid() &&
                        runtime.descriptorSet(dayo::graphics::NativeSceneDescriptorSet::textures).valid() &&
                        runtime.descriptorSet(dayo::graphics::NativeSceneDescriptorSet::rawVertices).valid(),
                    "native scene resource runtime exposes bound frame and array sets");
        auto incomplete = resources;
        incomplete.textures = std::span<const dayo::graphics::handles::TextureHandle>(textures.data(), 1);
        ok &= check(!runtime.sync(incomplete, &error) && !error.empty(),
                    "native scene resource runtime rejects an array count mismatch");
    }
    // The resource store supplies safe placeholders for every fixed binding,
    // but refuses to manufacture a fake TLAS for a missing scene geometry.
    {
        MockNativeDevice device;
        dayo::graphics::NativeSceneResourceStore store;
        const dayo::graphics::NativeSceneDescriptorCounts counts{.textures = 2};
        std::string error;
        ok &= check(store.initialize(device, counts, &error) && store.ready(),
                    "native scene resource store initializes fallback resources without a TLAS");
        const auto& fallback = store.bindings();
        const auto fallbackRtOutput = fallback.rtOutput;
        ok &= check(!fallback.tlas.valid() && fallback.rtOutput.valid() && fallback.oidnBuffer.valid() &&
                        fallback.textures.size() == 2 && fallback.textures[0].valid(),
                    "native scene resource store keeps TLAS empty while filling fixed placeholders");

        std::uint32_t nextHandle = 100;
        const auto texture = [&nextHandle]() { return dayo::graphics::handles::TextureHandle{nextHandle++, 1}; };
        const auto acceleration = [&nextHandle]() {
            return dayo::graphics::handles::AccelerationStructureHandle{nextHandle++, 1};
        };
        const std::array<dayo::graphics::handles::TextureHandle, 2> textures{texture(), texture()};
        dayo::graphics::NativeSceneResourceBindings overrides;
        overrides.tlas = acceleration();
        overrides.rtOutput = texture();
        overrides.textures = textures;
        ok &= check(store.compose(overrides, &error),
                    "native scene resource store composes a real TLAS and scene overrides");
        ok &= check(store.bindings().tlas == overrides.tlas && store.bindings().rtOutput == overrides.rtOutput &&
                        store.bindings().textures[0] == textures[0] && store.bindings().textures[1] == textures[1],
                    "native scene resource store retains owned array and scalar overrides");

        auto placeholderOverride = overrides;
        placeholderOverride.rtOutput = fallbackRtOutput;
        ok &= check(store.compose(placeholderOverride, &error) &&
                        (store.bindings().hostResourceMask &
                         dayo::graphics::dayoSemanticBit(dayo::graphics::DayoSemantic::RTOutput)) == 0,
                    "native scene resource store does not mark its placeholder as a real host semantic");

        auto invalid = overrides;
        invalid.textures = std::span<const dayo::graphics::handles::TextureHandle>(textures.data(), 1);
        ok &= check(!store.compose(invalid, &error) && !error.empty(),
                    "native scene resource store rejects an incomplete fixed array");
        store.reset();
        ok &= check(device.destroyedTextures == 1 && device.destroyedBuffers == 1,
                    "native scene resource store releases only its owned placeholders");
    }
    // DenoiserRuntime fallback: staging/readback -> CPU -> upload copy without CUDA.
    {
        dayo::core::DenoiserRuntime runtime;
        runtime.setShareable(false);
        const bool ensured = runtime.ensure(2, 1);
        ok &= check(ensured || !runtime.available(), "denoiser ensure accepts extent or reports unavailable");
        const std::array<float, 6> beauty{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
        const std::array<float, 6> albedo{0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F};
        const std::array<float, 6> normal{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
        std::array<float, 6> output{};
        const dayo::core::DenoiserExecuteArgs args{
            .width = 2, .height = 1, .beauty = beauty, .albedo = albedo, .normal = normal, .output = output};
        ok &= check(runtime.execute(args), "denoiser staging fallback executes");
        ok &= check(runtime.usedFallback(), "denoiser staging path marks fallback");
        if (runtime.available())
            ok &= check(runtime.denoised(), "available staging path runs CPU OIDN");
        else
            ok &= check(!runtime.denoised() && output == beauty, "unavailable denoiser is explicit passthrough");
        std::array<float, 6> badOutput{};
        const dayo::core::DenoiserExecuteArgs bad{
            .width = 4, .height = 4, .beauty = beauty, .albedo = albedo, .normal = normal, .output = badOutput};
        ok &= check(!runtime.execute(bad), "denoiser rejects mismatched oidnPass extent at execution");
        dayo::core::DenoiserRuntime forced;
        forced.setForceFallback(true);
        forced.setShareable(true);
        static_cast<void>(forced.ensure(2, 1));
        std::array<float, 6> forcedOutput{};
        const dayo::core::DenoiserExecuteArgs forcedArgs{
            .width = 2, .height = 1, .beauty = beauty, .albedo = {}, .normal = {}, .output = forcedOutput};
        ok &= check(forced.execute(forcedArgs), "denoiser forced CPU fallback executes");
        ok &= check(forced.usedFallback() && (!forced.available() ? forcedOutput == beauty : forced.denoised()),
                    "denoiser forced path distinguishes CPU denoise from passthrough");
    }
    // Feature gate: native flags stay false, Preview fallback remains available.
    {
        dayo::graphics::DeviceCapabilities capabilities;
        capabilities.swapchain = true;
        capabilities.bufferDeviceAddress = true;
        capabilities.descriptorIndexing = true;
        capabilities.accelerationStructure = true;
        capabilities.rayQuery = true;
        capabilities.fragmentShaderBarycentric = true;
        capabilities.rayTracingPipeline = true;
        capabilities.nativeSubayai = false;
        capabilities.nativeBdpt = false;
        ok &= check(!capabilities.supportsSubayai(), "native Subayai stays disabled");
        ok &= check(!capabilities.supportsBdpt(), "native BDPT stays disabled");
        ok &= check(capabilities.supportsPreview(), "Preview stays available without RT");
        ok &= check(capabilities.hardwareSupportsSubayai() && capabilities.hardwareSupportsBdpt(),
                    "hardware detection independent of native flags");
        ok &= check(AccelerationStructureService::canBuildNative(capabilities, dayo::graphics::RendererKind::bdpt),
                    "host bookkeeping allowed on RT hardware");
        dayo::graphics::DeviceCapabilities noRt;
        noRt.swapchain = true;
        ok &= check(!AccelerationStructureService::canBuildNative(noRt, dayo::graphics::RendererKind::subayai),
                    "RT-less GPU falls back to Preview");
        ok &= check(noRt.supportsPreview(), "RT-less GPU still starts Preview");
        ok &= check(!noRt.missingFeatures(dayo::graphics::RendererKind::subayai).empty(),
                    "missing RT features reported for fallback");
    }

    if (!ok) {
        return 1;
    }
    std::cout << "subayai_bdpt tests passed\n";
    return 0;
}
