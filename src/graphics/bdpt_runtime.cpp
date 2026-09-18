#include "graphics/bdpt_runtime.hpp"

#include "graphics/native_scene_bindings.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {

namespace {

[[nodiscard]] ShaderStageMask nativeBdptStages() noexcept {
    return ShaderStageMask::compute | ShaderStageMask::rayGeneration | ShaderStageMask::miss |
           ShaderStageMask::closestHit | ShaderStageMask::anyHit | ShaderStageMask::intersection |
           ShaderStageMask::callable;
}

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

} // namespace

DescriptorSetLayoutDesc bdptResourceBindingLayout() noexcept {
    const auto stages = nativeBdptStages();
    DescriptorSetLayoutDesc result;
    result.bindings.reserve(3U + BdptAccumulation::kVolumeSlots);
    result.bindings.push_back(
        {nativeSceneBinding(NativeSceneRegisterClass::uav, 0), DescriptorKind::storageImage, 1, stages});
    result.bindings.push_back(
        {nativeSceneBinding(NativeSceneRegisterClass::sampled, 0), DescriptorKind::storageBuffer, 1, stages});
    result.bindings.push_back(
        {nativeSceneBinding(NativeSceneRegisterClass::sampled, 1), DescriptorKind::storageBuffer, 1, stages});
    for (std::uint32_t index = 0; index < BdptAccumulation::kVolumeSlots; ++index)
        result.bindings.push_back(
            {nativeSceneBinding(NativeSceneRegisterClass::uav, 1U + index), DescriptorKind::storageImage, 1, stages});
    return result;
}

bool BdptRuntime::initialize(Device& device, fx::FxProgram program, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (!device.capabilities().hardwareSupportsBdpt()) {
        if (error != nullptr)
            *error = device.capabilities().missingFeatures(RendererKind::bdpt);
        return false;
    }
    const auto required = fx::requiredFeatures(program);
    if (!required.accelerationStructure || !required.rayTracingPipeline) {
        if (error != nullptr)
            *error = "BDPT graph does not declare the required acceleration-structure and RT-pipeline features";
        return false;
    }
    if (required.rayQuery && !device.capabilities().rayQuery) {
        if (error != nullptr)
            *error = "BDPT graph requires ray query support";
        return false;
    }
    device_ = &device;
    program_ = std::move(program);
    geometry_.setBackend(device.nativeAccelerationBackend());
    try {
        std::string bindingError;
        if (!bindings_.initialize(device, &bindingError))
            throw std::runtime_error(bindingError.empty() ? "BDPT shared bindings are unavailable" : bindingError);
        descriptorLayout_ = device.createDescriptorSetLayoutEx(bdptResourceBindingLayout());
        if (!descriptorLayout_.valid())
            throw std::runtime_error("BDPT resource descriptor layout is invalid");
    } catch (const std::exception& exception) {
        setError(error, std::string("BDPT descriptor layout initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "BDPT descriptor layout initialization failed");
        reset();
        return false;
    }
    ready_ = true;
    return true;
}

void BdptRuntime::reset() noexcept {
    nativeFx_.reset();
    geometry_.reset();
    bindings_.reset();
    lightRuntime_.reset();
    if (device_ != nullptr) {
        for (const auto descriptorSet : descriptorSets_) {
            if (descriptorSet.valid()) {
                try {
                    device_->destroyDescriptorSetEx(descriptorSet);
                } catch (...) {
                }
            }
        }
        if (descriptorLayout_.valid()) {
            try {
                device_->destroyDescriptorSetLayoutEx(descriptorLayout_);
            } catch (...) {
            }
        }
    }
    if (device_ != nullptr)
        accumulation_.releaseGpuResources(*device_);
    device_ = nullptr;
    program_ = {};
    accumulation_ = {};
    descriptorLayout_ = {};
    descriptorSets_.fill({});
    nativeAttempted_ = false;
    ready_ = false;
}

bool BdptRuntime::syncGeometry(std::span<const NativeGeometryMeshUpload> meshes, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready_ || device_ == nullptr) {
        setError(error, "BDPT runtime is not initialized");
        return false;
    }
    geometry_.setBackend(device_->nativeAccelerationBackend());
    if (meshes.empty()) {
        geometry_.reset();
        return true;
    }
    bool replaceGeometry = !geometry_.ready() || geometry_.meshCount() != meshes.size();
    if (!replaceGeometry) {
        for (const auto& mesh : meshes) {
            if (geometry_.deform(mesh.meshId) == nullptr) {
                replaceGeometry = true;
                break;
            }
        }
    }
    if (replaceGeometry) {
        geometry_.reset();
        geometry_.setBackend(device_->nativeAccelerationBackend());
        return geometry_.initialize(*device_, meshes, error);
    }
    return std::ranges::all_of(meshes, [this, error](const auto& mesh) { return geometry_.updateMesh(mesh, error); });
}

void BdptRuntime::recordGeometry(CommandList& commands) const {
    if (geometry_.ready())
        geometry_.recordDeform(commands);
}

void BdptRuntime::recordGeometry(CommandList& commands, std::span<const NativeGeometryMeshUpload> meshes) {
    if (geometry_.ready())
        geometry_.recordDeform(commands, meshes);
}

void BdptRuntime::recordAcceleration(CommandList& commands) const {
    if (geometry_.ready())
        geometry_.recordAcceleration(commands);
}

bool BdptRuntime::synchronizeAcceleration(std::string* error) {
    if (!geometry_.ready()) {
        setError(error, "BDPT geometry is not initialized");
        return false;
    }
    return geometry_.synchronizeAcceleration(error);
}

TlasAction BdptRuntime::synchronizeWorld(std::uint64_t worldGeneration, std::span<const WorldInstance> instances) {
    if (!geometry_.ready())
        throw std::logic_error("BDPT geometry is not initialized");
    return geometry_.synchronizeWorld(worldGeneration, instances);
}

bool BdptRuntime::ensureResources(std::uint32_t width, std::uint32_t height, std::string* error) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    if (!accumulation_.ensureGpuResources(*device_, width, height, 32, error))
        return false;
    const auto gpu = accumulation_.gpuResources();
    std::array<DescriptorBindingEx, 3U + BdptAccumulation::kVolumeSlots> bindings{};
    bindings[0] = {
        .slot = nativeSceneBinding(NativeSceneRegisterClass::uav, 0), .arrayElement = 0, .texture = gpu.accumulation};
    bindings[1] = {
        .slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 0), .arrayElement = 0, .buffer = gpu.spectralLut};
    bindings[2] = {.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 1),
                   .arrayElement = 0,
                   .buffer = gpu.blackbodyLut};
    for (std::size_t index = 0; index < BdptAccumulation::kVolumeSlots; ++index)
        bindings[3U + index] = {
            .slot = nativeSceneBinding(NativeSceneRegisterClass::uav, 1U + static_cast<std::uint32_t>(index)),
            .arrayElement = 0,
            .texture = gpu.volumes[index]};
    try {
        const auto slot = device_->currentFrameSlot() % kNativeFramesInFlight;
        if (descriptorSets_[slot].valid()) {
            device_->updateDescriptorSetEx(descriptorSets_[slot], bindings);
        } else {
            descriptorSets_[slot] = device_->allocateDescriptorSetEx(descriptorLayout_, bindings);
            if (!descriptorSets_[slot].valid())
                throw std::runtime_error("BDPT resource descriptor set is invalid");
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("BDPT resource descriptor binding failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "BDPT resource descriptor binding failed");
        return false;
    }
    return true;
}

BdptFrame BdptRuntime::prepareFrame(const fx::FxFrameContext& context, core::DirtyFlag dirty,
                                    std::span<const AliasEntry> lightSampling) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    std::string error;
    if (!ensureResources(context.renderWidth, context.renderHeight, &error))
        throw std::runtime_error(error.empty() ? "BDPT GPU resources are unavailable" : error);
    std::string lightError;
    if (!lightRuntime_.sync(*device_, lightSampling, &lightError))
        throw std::runtime_error(lightError.empty() ? "BDPT light sampling GPU upload failed" : lightError);
    if (!bindings_.bindLightSampling(lightRuntime_.buffer(), &lightError))
        throw std::runtime_error(lightError.empty() ? "BDPT light sampling descriptor binding failed" : lightError);
    BdptFrame frame;
    frame.clearAccumulation = accumulation_.beginFrame(dirty);
    frame.sampleIndex = accumulation_.sampleIndex();
    // The accumulation counter is owned by this runtime, not by the scene's
    // animation timeline. Publish the value selected for this output frame
    // before making the FX plan so ViewCB.output.z and SAMPLE expressions use
    // the same sample that will be accumulated by the native pass.
    frame.context = context;
    frame.context.sample = frame.sampleIndex;
    frame.plan = fx::FxCompiler{}.plan(program_, frame.context);
    frame.gpu = accumulation_.gpuResources();
    frame.descriptorSet = descriptorSets_[device_->currentFrameSlot() % kNativeFramesInFlight];
    frame.lightSamplingBuffer = lightRuntime_.buffer();
    frame.lightSamplingDescriptorSet = bindings_.lightSamplingSet();
    frame.geometryDescriptorSet = geometry_.descriptorSet();
    frame.usesCanonicalSceneBindings = sceneFrame_ != nullptr;
    if (!nativeAttempted_ && !program_.hlsl.empty()) {
        nativeAttempted_ = true;
        std::vector<handles::DescriptorSetLayoutHandle> sharedLayouts;
        std::vector<handles::DescriptorSetHandle> sharedSets;
        fx::FxNativeShaderSourceOptions sourceOptions;
        sourceOptions.preamble = "struct YRZ_BdptAliasEntry { float probability; uint alias; };\n";
        if (sceneFrame_ != nullptr) {
            if (!sceneFrame_->descriptorSetsReady())
                throw std::runtime_error("native BDPT scene descriptor sets are not synchronized");
            const auto layouts = sceneFrame_->layouts();
            const auto sets = sceneFrame_->descriptorSets();
            sharedLayouts.assign(layouts.begin(), layouts.end());
            sharedSets.assign(sets.begin(), sets.end());
        }
        const auto append = [&](handles::DescriptorSetLayoutHandle layout, handles::DescriptorSetHandle set,
                                auto&& describe) {
            if (!layout.valid() || !set.valid())
                return;
            const auto index = static_cast<std::uint32_t>(sharedLayouts.size());
            sharedLayouts.push_back(layout);
            sharedSets.push_back(set);
            describe(index);
        };
        append(bindings_.layouts().lightSampling, bindings_.lightSamplingSet(), [&](const auto index) {
            sourceOptions.resources.push_back(
                {.declaration = "StructuredBuffer<YRZ_BdptAliasEntry> YRZ_BdptLightSampling",
                 .registerClass = fx::FxNativeShaderRegister::sampled,
                 .registerIndex = 0,
                 .descriptorSet = index});
        });
        append(descriptorLayout_, descriptorSets_[device_->currentFrameSlot() % kNativeFramesInFlight],
               [&](const auto index) {
                   sourceOptions.resources.push_back({.declaration = "RWTexture2D<float4> YRZ_BdptAccumulation",
                                                      .registerClass = fx::FxNativeShaderRegister::uav,
                                                      .registerIndex = 0,
                                                      .descriptorSet = index});
                   sourceOptions.resources.push_back({.declaration = "StructuredBuffer<float> YRZ_BdptSpectralLut",
                                                      .registerClass = fx::FxNativeShaderRegister::sampled,
                                                      .registerIndex = 0,
                                                      .descriptorSet = index});
                   sourceOptions.resources.push_back({.declaration = "StructuredBuffer<float> YRZ_BdptBlackbodyLut",
                                                      .registerClass = fx::FxNativeShaderRegister::sampled,
                                                      .registerIndex = 1,
                                                      .descriptorSet = index});
                   for (std::uint32_t volume = 0; volume < BdptAccumulation::kVolumeSlots; ++volume)
                       sourceOptions.resources.push_back(
                           {.declaration = "RWTexture3D<float4> YRZ_BdptVolume" + std::to_string(volume),
                            .registerClass = fx::FxNativeShaderRegister::uav,
                            .registerIndex = 1U + volume,
                            .descriptorSet = index});
               });
        append(geometry_.descriptorLayout(), geometry_.descriptorSet(), [&](const auto index) {
            sourceOptions.resources.push_back({.declaration = "RaytracingAccelerationStructure YRZ_BdptTLAS",
                                               .registerClass = fx::FxNativeShaderRegister::sampled,
                                               .registerIndex = 0,
                                               .descriptorSet = index});
        });
        std::string nativeError;
        static_cast<void>(nativeFx_.initializeForFrame(*device_, program_, fx::FxShaderCompiler{}, context,
                                                       sharedLayouts, &nativeError, sharedSets,
                                                       std::move(sourceOptions)));
    } else if (nativeFx_.ready()) {
        std::string nativeError;
        static_cast<void>(nativeFx_.refresh(context, &nativeError));
    }
    if (nativeFx_.ready()) {
        std::vector<handles::DescriptorSetHandle> frameSharedSets;
        if (sceneFrame_) {
            const auto sets = sceneFrame_->descriptorSets();
            frameSharedSets.insert(frameSharedSets.end(), sets.begin(), sets.end());
        }
        const auto append = [&frameSharedSets](handles::DescriptorSetLayoutHandle layout,
                                               handles::DescriptorSetHandle set) {
            if (layout.valid() && set.valid())
                frameSharedSets.push_back(set);
        };
        append(bindings_.layouts().lightSampling, bindings_.lightSamplingSet());
        append(descriptorLayout_, descriptorSets_[device_->currentFrameSlot() % kNativeFramesInFlight]);
        append(geometry_.descriptorLayout(), geometry_.descriptorSet());
        frame.nativeFx = nativeFx_.prepareFrame(context, frameSharedSets);
    }
    return frame;
}

VulkanFxExecutor::Stats BdptRuntime::execute(BdptFrame& frame, CommandList& commands,
                                             const FxExecutionResources& resources) const {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    if (frame.clearAccumulation) {
        commands.transitionEx(frame.gpu.accumulation);
        commands.clearTextureEx(frame.gpu.accumulation);
        commands.memoryBarrierEx();
    }
    if (frame.nativeFx.has_value())
        return nativeFx_.execute(*frame.nativeFx, commands, resources);
    auto nativeResources = resources;
    if ((frame.descriptorSet.valid() || frame.lightSamplingDescriptorSet.valid() ||
         frame.geometryDescriptorSet.valid()) &&
        !nativeResources.resolveDescriptorSets && !nativeResources.resolveDescriptorSet) {
        const auto descriptorSet = frame.descriptorSet;
        const auto lightSamplingSet = frame.lightSamplingDescriptorSet;
        const auto geometrySet = frame.geometryDescriptorSet;
        nativeResources.resolveDescriptorSets = [descriptorSet, lightSamplingSet, geometrySet](const fx::FxDispatch&) {
            std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
            if (lightSamplingSet.valid())
                result.push_back({lightSamplingSet, 0});
            if (descriptorSet.valid())
                result.push_back({descriptorSet, lightSamplingSet.valid() ? 1U : 0U});
            if (geometrySet.valid())
                result.push_back({geometrySet, lightSamplingSet.valid() ? 2U : 1U});
            return result;
        };
    }
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, nativeResources);
}

std::optional<NativeFrameOutput> BdptRuntime::output(const BdptFrame& frame) const {
    if (frame.nativeFx.has_value() && nativeFx_.ready()) {
        if (const auto output = nativeFx_.output(*frame.nativeFx); output.has_value())
            return output;
    }
    if (!frame.gpu.accumulation.valid())
        return std::nullopt;
    return NativeFrameOutput{.texture = frame.gpu.accumulation,
                             .extent = {frame.gpu.width, frame.gpu.height, 1},
                             .format = PixelFormat::rgba16Float};
}

} // namespace dayo::graphics
