#include "graphics/subayai_runtime.hpp"

#include "graphics/native_renderer_requirements.hpp"

#include <algorithm>

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
    std::string bindingError;
    if (!bindings_.initialize(device, &bindingError)) {
        if (error != nullptr)
            *error = bindingError.empty() ? "Subayai descriptor bindings are unavailable" : bindingError;
        reset();
        return false;
    }
    ready_ = true;
    return true;
}

void SubayaiRuntime::reset() noexcept {
    bindings_.reset();
    materialRuntime_.reset();
    lightRuntime_.reset();
    device_ = nullptr;
    program_ = {};
    materials_.clear();
    ready_ = false;
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
    SubayaiFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context);
    frame.materials = materials_;
    frame.materialBuffer = materialRuntime_.buffer();
    frame.materialDescriptorSet = bindings_.materialSet();
    frame.lightSampling.assign(lightSampling.begin(), lightSampling.end());
    frame.lightSamplingBuffer = lightRuntime_.buffer();
    frame.lightSamplingDescriptorSet = bindings_.lightSamplingSet();
    frame.environment = environment;
    return frame;
}

VulkanFxExecutor::Stats SubayaiRuntime::execute(SubayaiFrame& frame, CommandList& commands,
                                                const FxExecutionResources& resources) const {
    if (!ready_)
        throw std::logic_error("Subayai runtime is not initialized");
    auto nativeResources = resources;
    if (!nativeResources.resolveDescriptorSets && !nativeResources.resolveDescriptorSet &&
        (frame.materialDescriptorSet.valid() || frame.lightSamplingDescriptorSet.valid())) {
        const auto materialSet = frame.materialDescriptorSet;
        const auto lightSet = frame.lightSamplingDescriptorSet;
        nativeResources.resolveDescriptorSets =
            [materialSet, lightSet](const fx::FxDispatch&) {
                std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
                if (materialSet.valid())
                    result.push_back({materialSet, 0});
                if (lightSet.valid())
                    result.push_back({lightSet, 1});
                return result;
            };
    }
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, nativeResources);
}

} // namespace dayo::graphics
