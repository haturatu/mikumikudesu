#include "graphics/subayai_runtime.hpp"

#include "graphics/native_renderer_requirements.hpp"

#include <algorithm>
#include <utility>

namespace dayo::graphics {

bool SubayaiRuntime::initialize(Device& device, fx::FxProgram program, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (!device.capabilities().hardwareSupportsSubayai()) {
        if (error != nullptr)
            *error = device.capabilities().missingFeatures(RendererKind::subayai);
        return false;
    }
    const auto required = fx::requiredFeatures(program);
    const auto missing = missingEffectFeatures(device.capabilities(), required);
    if (!missing.empty()) {
        if (error != nullptr)
            *error = "Subayai graph requires unavailable features: " + missing;
        return false;
    }
    device_ = &device;
    program_ = std::move(program);
    geometry_.setBackend(device.nativeAccelerationBackend());
    std::string bindingError;
    if (!bindings_.initialize(device, &bindingError)) {
        if (error != nullptr)
            *error = bindingError.empty() ? "Subayai descriptor bindings are unavailable" : bindingError;
        reset();
        return false;
    }
    if (!environmentRuntime_.initialize(device, &bindingError)) {
        if (error != nullptr)
            *error = bindingError.empty() ? "Subayai environment bindings are unavailable" : bindingError;
        reset();
        return false;
    }
    ready_ = true;
    return true;
}

void SubayaiRuntime::reset() noexcept {
    nativeFx_.reset();
    geometry_.reset();
    environmentRuntime_.reset();
    bindings_.reset();
    materialRuntime_.reset();
    lightRuntime_.reset();
    device_ = nullptr;
    program_ = {};
    materials_.clear();
    nativeAttempted_ = false;
    ready_ = false;
}

bool SubayaiRuntime::updateEnvironment(const EnvironmentDesc& description) {
    if (!ready_)
        return false;
    return environmentService_.update(description);
}

bool SubayaiRuntime::syncGeometry(std::span<const NativeGeometryMeshUpload> meshes, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready_ || device_ == nullptr) {
        if (error != nullptr)
            *error = "Subayai runtime is not initialized";
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
    for (const auto& mesh : meshes)
        if (!geometry_.updateMesh(mesh, error))
            return false;
    return true;
}

void SubayaiRuntime::recordGeometry(CommandList& commands) const {
    if (geometry_.ready())
        geometry_.recordDeform(commands);
}

bool SubayaiRuntime::synchronizeAcceleration(std::string* error) {
    if (!geometry_.ready()) {
        if (error != nullptr)
            *error = "Subayai geometry is not initialized";
        return false;
    }
    return geometry_.synchronizeAcceleration(error);
}

TlasAction SubayaiRuntime::synchronizeWorld(std::uint64_t worldGeneration,
                                             std::span<const WorldInstance> instances) {
    if (!geometry_.ready())
        throw std::logic_error("Subayai geometry is not initialized");
    return geometry_.synchronizeWorld(worldGeneration, instances);
}

bool SubayaiRuntime::syncMaterials(std::span<const core::MaterialParameterBlock> materials) {
    if (!ready_)
        return false;
    std::string error;
    if (!materialRuntime_.sync(*device_, materials, &error) ||
        !bindings_.bindMaterial(materialRuntime_.buffer(), &error))
        return false;
    const auto linked = materialRuntime_.materials();
    materials_.assign(linked.begin(), linked.end());
    return true;
}

SubayaiFrame SubayaiRuntime::prepareFrame(const fx::FxFrameContext& context,
                                          std::span<const core::MaterialParameterBlock> materials,
                                          std::span<const AliasEntry> lightSampling,
                                          const EnvironmentGpuResult& environment) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("Subayai runtime is not initialized");
    if (!materials.empty() && !syncMaterials(materials))
        throw std::runtime_error("Subayai material GPU upload failed");
    std::string lightError;
    if (!lightRuntime_.sync(*device_, lightSampling, &lightError))
        throw std::runtime_error(lightError.empty() ? "Subayai light sampling GPU upload failed" : lightError);
    if (!bindings_.bindLightSampling(lightRuntime_.buffer(), &lightError))
        throw std::runtime_error(lightError.empty() ? "Subayai light sampling descriptor binding failed" : lightError);
    const auto selectedEnvironment = environment.cubemap.valid() || environment.prefiltered.valid()
                                         ? environment
                                         : environmentService_.gpuResult();
    std::string environmentError;
    if (!environmentRuntime_.sync(*device_, selectedEnvironment, &environmentError))
        throw std::runtime_error(environmentError.empty() ? "Subayai environment binding failed" : environmentError);
    SubayaiFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context);
    frame.materials = materials_;
    frame.materialBuffer = materialRuntime_.buffer();
    frame.materialDescriptorSet = bindings_.materialSet();
    frame.lightSampling.assign(lightSampling.begin(), lightSampling.end());
    frame.lightSamplingBuffer = lightRuntime_.buffer();
    frame.lightSamplingDescriptorSet = bindings_.lightSamplingSet();
    frame.geometryDescriptorSet = geometry_.descriptorSet();
    frame.environment = selectedEnvironment;
    frame.environmentDescriptorSet = environmentRuntime_.descriptorSet();
    if (!nativeAttempted_ && !program_.hlsl.empty()) {
        nativeAttempted_ = true;
        std::vector<handles::DescriptorSetLayoutHandle> sharedLayouts{bindings_.layouts().material,
                                                                       bindings_.layouts().lightSampling,
                                                                       environmentRuntime_.layout()};
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

VulkanFxExecutor::Stats SubayaiRuntime::execute(SubayaiFrame& frame, CommandList& commands,
                                                const FxExecutionResources& resources) const {
    if (!ready_)
        throw std::logic_error("Subayai runtime is not initialized");
    if (frame.nativeFx.has_value()) {
        auto nativeResources = resources;
        if (frame.materialDescriptorSet.valid() || frame.lightSamplingDescriptorSet.valid() ||
            frame.geometryDescriptorSet.valid() || frame.environmentDescriptorSet.valid()) {
            const auto existingSets = nativeResources.resolveDescriptorSets;
            const auto existingSingle = nativeResources.resolveDescriptorSet;
            const auto materialSet = frame.materialDescriptorSet;
            const auto lightSet = frame.lightSamplingDescriptorSet;
            const auto geometrySet = frame.geometryDescriptorSet;
            const auto environmentSet = frame.environmentDescriptorSet;
            const auto environmentSetIndex = geometry_.descriptorLayout().valid() ? 3U : 2U;
            nativeResources.resolveDescriptorSets =
                [existingSets, existingSingle, materialSet, lightSet, geometrySet,
                 environmentSet, environmentSetIndex](const fx::FxDispatch& dispatch) {
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
                    if (materialSet.valid() && !hasIndex(0))
                        result.push_back({materialSet, 0});
                    if (lightSet.valid() && !hasIndex(1))
                        result.push_back({lightSet, 1});
                    if (geometrySet.valid() && !hasIndex(2))
                        result.push_back({geometrySet, 2});
                    if (environmentSet.valid() && !hasIndex(environmentSetIndex))
                        result.push_back({environmentSet, environmentSetIndex});
                    return result;
                };
            nativeResources.resolveDescriptorSet = {};
        }
        return nativeFx_.execute(*frame.nativeFx, commands, nativeResources);
    }
    auto nativeResources = resources;
    if (!nativeResources.resolveDescriptorSets && !nativeResources.resolveDescriptorSet &&
        (frame.materialDescriptorSet.valid() || frame.lightSamplingDescriptorSet.valid() ||
         frame.geometryDescriptorSet.valid() || frame.environmentDescriptorSet.valid())) {
        const auto materialSet = frame.materialDescriptorSet;
        const auto lightSet = frame.lightSamplingDescriptorSet;
        const auto geometrySet = frame.geometryDescriptorSet;
        const auto environmentSet = frame.environmentDescriptorSet;
        const auto environmentSetIndex = geometry_.descriptorLayout().valid() ? 3U : 2U;
        nativeResources.resolveDescriptorSets =
            [materialSet, lightSet, geometrySet, environmentSet, environmentSetIndex](const fx::FxDispatch&) {
                std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
                if (materialSet.valid())
                    result.push_back({materialSet, 0});
                if (lightSet.valid())
                    result.push_back({lightSet, 1});
                if (geometrySet.valid())
                    result.push_back({geometrySet, 2});
                if (environmentSet.valid())
                    result.push_back({environmentSet, environmentSetIndex});
                return result;
            };
    }
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, nativeResources);
}

std::optional<NativeFrameOutput> SubayaiRuntime::output(const SubayaiFrame& frame) const {
    if (!frame.nativeFx.has_value() || !nativeFx_.ready())
        return std::nullopt;
    return nativeFx_.output(*frame.nativeFx);
}

} // namespace dayo::graphics
