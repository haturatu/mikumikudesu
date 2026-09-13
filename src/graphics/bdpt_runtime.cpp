#include "graphics/bdpt_runtime.hpp"

#include <algorithm>
#include <array>
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
    result.bindings.push_back({0, DescriptorKind::storageImage, 1, stages});
    result.bindings.push_back({1, DescriptorKind::storageBuffer, 1, stages});
    result.bindings.push_back({2, DescriptorKind::storageBuffer, 1, stages});
    for (std::uint32_t index = 0; index < BdptAccumulation::kVolumeSlots; ++index)
        result.bindings.push_back({3U + index, DescriptorKind::storageImage, 1, stages});
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
    if (device_ != nullptr) {
        if (descriptorSet_.valid()) {
            try {
                device_->destroyDescriptorSetEx(descriptorSet_);
            } catch (...) {
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
    descriptorSet_ = {};
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
    if (!geometry_.ready())
        return geometry_.initialize(*device_, meshes, error);
    for (const auto& mesh : meshes)
        if (!geometry_.updateMesh(mesh, error))
            return false;
    return true;
}

void BdptRuntime::recordGeometry(CommandList& commands) const {
    if (geometry_.ready())
        geometry_.recordDeform(commands);
}

bool BdptRuntime::synchronizeAcceleration(std::string* error) {
    if (!geometry_.ready()) {
        setError(error, "BDPT geometry is not initialized");
        return false;
    }
    return geometry_.synchronizeAcceleration(error);
}

TlasAction BdptRuntime::synchronizeWorld(std::uint64_t worldGeneration,
                                         std::span<const WorldInstance> instances) {
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
    bindings[0] = {.slot = 0, .arrayElement = 0, .texture = gpu.accumulation};
    bindings[1] = {.slot = 1, .arrayElement = 0, .buffer = gpu.spectralLut};
    bindings[2] = {.slot = 2, .arrayElement = 0, .buffer = gpu.blackbodyLut};
    for (std::size_t index = 0; index < BdptAccumulation::kVolumeSlots; ++index)
        bindings[3U + index] = {.slot = static_cast<std::uint32_t>(3U + index),
                                 .arrayElement = 0,
                                 .texture = gpu.volumes[index]};
    try {
        if (descriptorSet_.valid()) {
            device_->updateDescriptorSetEx(descriptorSet_, bindings);
        } else {
            descriptorSet_ = device_->allocateDescriptorSetEx(descriptorLayout_, bindings);
            if (!descriptorSet_.valid())
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

BdptFrame BdptRuntime::prepareFrame(const fx::FxFrameContext& context, core::DirtyFlag dirty) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    std::string error;
    if (!ensureResources(context.renderWidth, context.renderHeight, &error))
        throw std::runtime_error(error.empty() ? "BDPT GPU resources are unavailable" : error);
    BdptFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context);
    frame.clearAccumulation = accumulation_.beginFrame(dirty);
    frame.sampleIndex = accumulation_.sampleIndex();
    frame.gpu = accumulation_.gpuResources();
    frame.descriptorSet = descriptorSet_;
    frame.geometryDescriptorSet = geometry_.descriptorSet();
    if (!nativeAttempted_ && !program_.hlsl.empty()) {
        nativeAttempted_ = true;
        std::vector<handles::DescriptorSetLayoutHandle> sharedLayouts{descriptorLayout_};
        if (geometry_.descriptorLayout().valid())
            sharedLayouts.push_back(geometry_.descriptorLayout());
        std::string nativeError;
        static_cast<void>(nativeFx_.initializeForFrame(*device_, program_, fx::FxShaderCompiler{}, context,
                                                       sharedLayouts, &nativeError));
    } else if (nativeFx_.ready()) {
        std::string nativeError;
        static_cast<void>(nativeFx_.refresh(context, &nativeError));
    }
    if (nativeFx_.ready())
        frame.nativeFx = nativeFx_.prepareFrame(context);
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
    if (frame.nativeFx.has_value()) {
        auto nativeResources = resources;
        const auto existingSets = nativeResources.resolveDescriptorSets;
        const auto existingSingle = nativeResources.resolveDescriptorSet;
        const auto accumulationSet = frame.descriptorSet;
        const auto geometrySet = frame.geometryDescriptorSet;
        nativeResources.resolveDescriptorSets =
            [existingSets, existingSingle, accumulationSet, geometrySet](const fx::FxDispatch& dispatch) {
                std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
                if (existingSets) {
                    result = existingSets(dispatch);
                } else if (existingSingle) {
                    const auto shared = existingSingle(dispatch);
                    if (shared.has_value())
                        result.push_back({*shared, 0});
                }
                const auto hasIndex = [&result](std::uint32_t index) {
                    return std::any_of(result.begin(), result.end(), [index](const auto& binding) {
                        return binding.setIndex == index;
                    });
                };
                if (accumulationSet.valid() && !hasIndex(0))
                    result.push_back({accumulationSet, 0});
                if (geometrySet.valid() && !hasIndex(1))
                    result.push_back({geometrySet, 1});
                return result;
            };
        nativeResources.resolveDescriptorSet = {};
        return nativeFx_.execute(*frame.nativeFx, commands, nativeResources);
    }
    auto nativeResources = resources;
    if ((frame.descriptorSet.valid() || frame.geometryDescriptorSet.valid()) &&
        !nativeResources.resolveDescriptorSets && !nativeResources.resolveDescriptorSet) {
        const auto descriptorSet = frame.descriptorSet;
        const auto geometrySet = frame.geometryDescriptorSet;
        nativeResources.resolveDescriptorSets = [descriptorSet, geometrySet](const fx::FxDispatch&) {
            std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
            if (descriptorSet.valid())
                result.push_back({descriptorSet, 0});
            if (geometrySet.valid())
                result.push_back({geometrySet, 1});
            return result;
        };
    }
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, nativeResources);
}

std::optional<NativeFrameOutput> BdptRuntime::output(const BdptFrame& frame) const {
    if (!frame.nativeFx.has_value() || !nativeFx_.ready())
        return std::nullopt;
    return nativeFx_.output(*frame.nativeFx);
}

} // namespace dayo::graphics
