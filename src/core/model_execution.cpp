#include "core/model_execution.hpp"

#include <algorithm>

namespace dayo::core {

ModelExecutionPolicy ModelExecutionPlanner::resolve(const ModelInstance& model,
                                                    const SceneEffectStack& effects) noexcept {
    const auto ownedByModel = [&model](const SceneEffectInstance& effect) {
        return effect.controllerModel.has_value() && *effect.controllerModel == model.id;
    };
    const bool postprocessLauncher = std::ranges::any_of(effects.postprocess, ownedByModel);
    const bool assignedDeformer = std::ranges::any_of(effects.deform, ownedByModel);
    const bool validModel = model.model != nullptr && model.animator != nullptr;
    const bool farController = validModel && !model.upstreamDrawable;
    const bool visible = model.visible && model.animationVisible;
    const bool drawable = validModel && !farController;

    ModelExecutionPolicy policy;
    policy.role = postprocessLauncher ? ModelRuntimeRole::postprocessLauncher
                                      : (farController ? ModelRuntimeRole::controllerOnly : ModelRuntimeRole::scene);
    policy.evaluateAnimation = validModel;
    policy.evaluatePhysics = validModel && model.physics != nullptr;
    policy.runDeformer = drawable && assignedDeformer && !postprocessLauncher;
    policy.rasterize = drawable && visible && !postprocessLauncher;
    policy.buildBlas = drawable && !postprocessLauncher;
    policy.includeInTlas = policy.rasterize;
    policy.exposeToControllers = validModel;
    policy.visible = visible;
    policy.farController = farController;
    return policy;
}

std::vector<ModelExecutionPolicy> ModelExecutionPlanner::plan(std::span<const ModelInstance> models,
                                                              const SceneEffectStack& effects) {
    std::vector<ModelExecutionPolicy> result;
    result.reserve(models.size());
    for (const auto& model : models)
        result.push_back(resolve(model, effects));
    return result;
}

} // namespace dayo::core
