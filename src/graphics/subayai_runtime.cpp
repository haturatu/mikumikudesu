#include "graphics/subayai_runtime.hpp"

#include "graphics/native_renderer_requirements.hpp"

#include <algorithm>
#include <utility>

namespace dayo::graphics {

namespace {

void addSharedResource(fx::FxNativeShaderSourceOptions& options, std::string declaration,
                       fx::FxNativeShaderRegister registerClass, std::uint32_t registerIndex,
                       std::uint32_t descriptorSet) {
    options.resources.push_back({.declaration = std::move(declaration),
                                 .registerClass = registerClass,
                                 .registerIndex = registerIndex,
                                 .descriptorSet = descriptorSet});
}

std::string subayaiShaderPreamble() {
    return "struct YRZ_SubayaiMaterialGpu {\n"
           "    float4 baseColor;\n"
           "    float4 emission;\n"
           "    float4 specular;\n"
           "    float4 hair;\n"
           "    float4 surface;\n"
           "    uint4 flags;\n"
           "};\n"
           "struct YRZ_SubayaiAliasEntry { float probability; uint alias; };\n";
}

void appendSubayaiSharedBindings(const SubayaiBindingRuntime& bindings, const SubayaiEnvironmentRuntime& environment,
                                 const NativeGeometryRuntime& geometry,
                                 std::vector<handles::DescriptorSetLayoutHandle>& layouts,
                                 std::vector<handles::DescriptorSetHandle>& descriptorSets,
                                 fx::FxNativeShaderSourceOptions& options) {
    const auto append = [&](handles::DescriptorSetLayoutHandle layout, handles::DescriptorSetHandle set,
                            const auto& describe) {
        if (!layout.valid() || !set.valid())
            return;
        const auto index = static_cast<std::uint32_t>(layouts.size());
        layouts.push_back(layout);
        descriptorSets.push_back(set);
        describe(index);
    };
    append(bindings.layouts().material, bindings.materialSet(), [&](const auto index) {
        addSharedResource(options, "StructuredBuffer<YRZ_SubayaiMaterialGpu> YRZ_SubayaiMaterials",
                          fx::FxNativeShaderRegister::sampled, 0, index);
    });
    append(bindings.layouts().lightSampling, bindings.lightSamplingSet(), [&](const auto index) {
        addSharedResource(options, "StructuredBuffer<YRZ_SubayaiAliasEntry> YRZ_SubayaiLightSampling",
                          fx::FxNativeShaderRegister::sampled, 0, index);
    });
    append(environment.layout(), environment.descriptorSet(), [&](const auto index) {
        addSharedResource(options, "TextureCube<float4> YRZ_SubayaiEnvironment", fx::FxNativeShaderRegister::sampled, 0,
                          index);
        addSharedResource(options, "TextureCube<float4> YRZ_SubayaiPrefilteredEnvironment",
                          fx::FxNativeShaderRegister::sampled, 1, index);
        addSharedResource(options, "StructuredBuffer<float> YRZ_SubayaiEnvironmentSH",
                          fx::FxNativeShaderRegister::sampled, 2, index);
    });
    append(geometry.descriptorLayout(), geometry.descriptorSet(), [&](const auto index) {
        addSharedResource(options, "RaytracingAccelerationStructure YRZ_SubayaiTLAS",
                          fx::FxNativeShaderRegister::sampled, 0, index);
    });
}

} // namespace

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
    dayoFx_.reset();
    materialSceneRuntime_.reset();
    geometry_.reset();
    environmentRuntime_.reset();
    bindings_.reset();
    materialRuntime_.reset();
    lightRuntime_.reset();
    device_ = nullptr;
    environmentService_ = EnvironmentService(nullptr);
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
    return std::ranges::all_of(meshes, [this, error](const auto& mesh) { return geometry_.updateMesh(mesh, error); });
}

void SubayaiRuntime::recordGeometry(CommandList& commands) const {
    if (geometry_.ready())
        geometry_.recordDeform(commands);
}

void SubayaiRuntime::recordGeometry(CommandList& commands, std::span<const NativeGeometryMeshUpload> meshes) {
    if (geometry_.ready())
        geometry_.recordDeform(commands, meshes);
}

void SubayaiRuntime::recordAcceleration(CommandList& commands) const {
    if (geometry_.ready())
        geometry_.recordAcceleration(commands);
}

bool SubayaiRuntime::synchronizeAcceleration(std::string* error) {
    if (!geometry_.ready()) {
        if (error != nullptr)
            *error = "Subayai geometry is not initialized";
        return false;
    }
    return geometry_.synchronizeAcceleration(error);
}

TlasAction SubayaiRuntime::synchronizeWorld(std::uint64_t worldGeneration, std::span<const WorldInstance> instances) {
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
                                          const EnvironmentGpuResult& environment,
                                          std::span<const FxMaterialSceneModel> materialModels) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("Subayai runtime is not initialized");
    const FxMaterialGpuRuntime* materialGpuRuntime = nullptr;
    if (program_.materialSchema.has_value()) {
        std::string materialError;
        if (!materialSceneRuntime_.sync(*device_, *program_.materialSchema, materialModels, context, &materialError))
            throw std::runtime_error(materialError.empty() ? "Subayai MatDesc synchronization failed" : materialError);
        materialGpuRuntime = &materialSceneRuntime_.gpuRuntime();
        if (materialSceneRuntime_.descriptorLayoutChanged() && nativeAttempted_) {
            dayoFx_.reset();
            nativeAttempted_ = false;
        }
    }
    if (!materials.empty() && !syncMaterials(materials))
        throw std::runtime_error("Subayai material GPU upload failed");
    std::string lightError;
    if (!lightRuntime_.sync(*device_, lightSampling, &lightError))
        throw std::runtime_error(lightError.empty() ? "Subayai light sampling GPU upload failed" : lightError);
    if (!bindings_.bindLightSampling(lightRuntime_.buffer(), &lightError))
        throw std::runtime_error(lightError.empty() ? "Subayai light sampling descriptor binding failed" : lightError);
    const auto selectedEnvironment =
        environment.cubemap.valid() || environment.prefiltered.valid() ? environment : environmentService_.gpuResult();
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
    frame.usesCanonicalSceneBindings = sceneFrame_ != nullptr;
    if (!nativeAttempted_ && !program_.hlsl.empty()) {
        nativeAttempted_ = true;
        std::vector<handles::DescriptorSetLayoutHandle> sharedLayouts;
        std::vector<handles::DescriptorSetHandle> sharedSets;
        fx::FxNativeShaderSourceOptions sourceOptions;
        if (sceneFrame_ != nullptr) {
            if (!sceneFrame_->descriptorSetsReady())
                throw std::runtime_error("native Subayai scene descriptor sets are not synchronized");
            const auto layouts = sceneFrame_->layouts();
            const auto sets = sceneFrame_->descriptorSets();
            sharedLayouts.assign(layouts.begin(), layouts.end());
            sharedSets.assign(sets.begin(), sets.end());
        }
        sourceOptions.preamble = subayaiShaderPreamble();
        sourceOptions.controllerDeclarations = controllerDeclarations_;
        appendSubayaiSharedBindings(bindings_, environmentRuntime_, geometry_, sharedLayouts, sharedSets,
                                    sourceOptions);
        std::string nativeError;
        if (externalResourceProvider_ != nullptr)
            dayoFx_.addProvider(*externalResourceProvider_);
        static_cast<void>(dayoFx_.initializeForFrame(*device_, program_, fx::FxShaderCompiler{}, context, sharedLayouts,
                                                     &nativeError, sharedSets, std::move(sourceOptions),
                                                     materialGpuRuntime));
    } else if (dayoFx_.ready()) {
        std::string nativeError;
        static_cast<void>(dayoFx_.refresh(context, &nativeError));
    }
    if (dayoFx_.ready()) {
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
        append(bindings_.layouts().material, bindings_.materialSet());
        append(bindings_.layouts().lightSampling, bindings_.lightSamplingSet());
        append(geometry_.descriptorLayout(), geometry_.descriptorSet());
        append(environmentRuntime_.layout(), environmentRuntime_.descriptorSet());
        frame.nativeFx = dayoFx_.prepareFrame(context, frameSharedSets);
    }
    return frame;
}

VulkanFxExecutor::Stats SubayaiRuntime::execute(SubayaiFrame& frame, CommandList& commands,
                                                const FxExecutionResources& resources) const {
    if (!ready_)
        throw std::logic_error("Subayai runtime is not initialized");
    if (frame.nativeFx.has_value())
        return dayoFx_.execute(*frame.nativeFx, commands, resources);
    auto nativeResources = resources;
    if (!nativeResources.resolveDescriptorSets && !nativeResources.resolveDescriptorSet &&
        (frame.materialDescriptorSet.valid() || frame.lightSamplingDescriptorSet.valid() ||
         frame.geometryDescriptorSet.valid() || frame.environmentDescriptorSet.valid())) {
        const auto materialSet = frame.materialDescriptorSet;
        const auto lightSet = frame.lightSamplingDescriptorSet;
        const auto geometrySet = frame.geometryDescriptorSet;
        const auto environmentSet = frame.environmentDescriptorSet;
        const auto environmentSetIndex = geometry_.descriptorLayout().valid() ? 3U : 2U;
        nativeResources.resolveDescriptorSets = [materialSet, lightSet, geometrySet, environmentSet,
                                                 environmentSetIndex](const fx::FxDispatch&) {
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
    if (!frame.nativeFx.has_value() || !dayoFx_.ready())
        return std::nullopt;
    return dayoFx_.output(*frame.nativeFx);
}

} // namespace dayo::graphics
