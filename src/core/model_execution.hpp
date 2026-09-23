#pragma once

#include "core/scene.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace dayo::core {

enum class ModelRuntimeRole : std::uint8_t {
    scene,
    controllerOnly,
    postprocessLauncher,
};

struct ModelExecutionPolicy {
    ModelRuntimeRole role{ModelRuntimeRole::scene};

    bool evaluateAnimation{true};
    bool evaluatePhysics{true};
    bool runDeformer{true};

    bool rasterize{true};
    bool buildBlas{true};
    bool includeInTlas{true};

    bool exposeToControllers{true};
    bool visible{true};
    bool farController{};
};

class ModelExecutionPlanner {
  public:
    [[nodiscard]] static ModelExecutionPolicy resolve(const ModelInstance& model,
                                                      const SceneEffectStack& effects) noexcept;
    [[nodiscard]] static std::vector<ModelExecutionPolicy> plan(std::span<const ModelInstance> models,
                                                                const SceneEffectStack& effects);
};

} // namespace dayo::core
