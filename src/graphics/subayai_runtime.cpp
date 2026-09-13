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
    ready_ = true;
    return true;
}

void SubayaiRuntime::reset() noexcept {
    device_ = nullptr;
    program_ = {};
    materials_.clear();
    ready_ = false;
}

bool SubayaiRuntime::syncMaterials(std::span<const core::MaterialParameterBlock> materials) {
    if (!ready_)
        return false;
    materials_.clear();
    materials_.reserve(materials.size());
    for (const auto& material : materials)
        materials_.push_back(linkSubayaiMaterial(material));
    return true;
}

SubayaiFrame SubayaiRuntime::prepareFrame(const fx::FxFrameContext& context,
                                          std::span<const core::MaterialParameterBlock> materials,
                                          std::span<const AliasEntry> lightSampling,
                                          const EnvironmentGpuResult& environment) const {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("Subayai runtime is not initialized");
    SubayaiFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context);
    frame.materials = materials_.empty() ? std::vector<SubayaiMaterialGpu>{} : materials_;
    if (!materials.empty()) {
        frame.materials.clear();
        frame.materials.reserve(materials.size());
        for (const auto& material : materials)
            frame.materials.push_back(linkSubayaiMaterial(material));
    }
    frame.lightSampling.assign(lightSampling.begin(), lightSampling.end());
    frame.environment = environment;
    return frame;
}

VulkanFxExecutor::Stats SubayaiRuntime::execute(SubayaiFrame& frame, CommandList& commands,
                                                const FxExecutionResources& resources) const {
    if (!ready_)
        throw std::logic_error("Subayai runtime is not initialized");
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, resources);
}

} // namespace dayo::graphics
