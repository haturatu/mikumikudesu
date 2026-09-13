#include "graphics/subayai_runtime.hpp"

#include "graphics/native_renderer_requirements.hpp"

#include <algorithm>
#include <array>
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
    nativeFx_.reset();
    materialRuntime_.reset();
    lightRuntime_.reset();
    device_ = nullptr;
    program_ = {};
    materials_.clear();
    nativeAttempted_ = false;
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
    if (!nativeAttempted_ && !program_.hlsl.empty()) {
        nativeAttempted_ = true;
        const std::array sharedLayouts{bindings_.layouts().material, bindings_.layouts().lightSampling};
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
        if (frame.materialDescriptorSet.valid() || frame.lightSamplingDescriptorSet.valid()) {
            const auto existingSets = nativeResources.resolveDescriptorSets;
            const auto existingSingle = nativeResources.resolveDescriptorSet;
            const auto materialSet = frame.materialDescriptorSet;
            const auto lightSet = frame.lightSamplingDescriptorSet;
            nativeResources.resolveDescriptorSets =
                [existingSets, existingSingle, materialSet, lightSet](const fx::FxDispatch& dispatch) {
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
                    return result;
                };
            nativeResources.resolveDescriptorSet = {};
        }
        return nativeFx_.execute(*frame.nativeFx, commands, nativeResources);
    }
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
