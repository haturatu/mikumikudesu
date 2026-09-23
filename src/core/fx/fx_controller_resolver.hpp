#pragma once

#include "core/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dayo::core::fx {

struct EvaluatedModelState {
    dayo::core::ModelId id{};
    std::filesystem::path sourcePath;
    std::string displayName;
    std::string modelName;
    std::string englishName;
    std::vector<std::string> morphNames;
    std::vector<std::string> boneNames;
    std::vector<float> morphWeights;
    std::vector<mmd::AnimatedModelFrame::BoneTransform> bones;
};

struct SceneEvaluationSnapshot {
    std::vector<EvaluatedModelState> models;
};

using FxControllerValue = std::variant<bool, std::int32_t, std::uint32_t, float, std::array<float, 2>,
                                       std::array<float, 3>, std::array<float, 4>, std::array<float, 16>>;

struct FxMorphControllerUi {
    std::optional<EffectSlider> slider;
    std::vector<std::string> descriptions;
};

// Finds the last active float-morph controller declaration for a model and
// returns its UI metadata. This does not alter controller values or animation.
[[nodiscard]] std::optional<FxMorphControllerUi>
resolveFxMorphControllerUi(const SceneEffectStack& effects, const ModelInstance& model, std::string_view morphName);

// Resolves EffectController declarations against a frame-stable PMX
// evaluation snapshot. It never reaches into a live animator, which keeps
// controller values coherent with the geometry evaluated for the same frame.
class FxControllerResolver {
  public:
    [[nodiscard]] FxControllerValue resolve(const EffectController& controller, const SceneEvaluationSnapshot& snapshot,
                                            dayo::core::ModelId self) const;
    // Resolves up to arrayCapacity models in evaluation/scene order. A scalar
    // controller still requires a unique target; array declarations permit
    // same-name PMX instances to map to successive elements.
    [[nodiscard]] std::vector<FxControllerValue> resolveArray(const EffectController& controller,
                                                              const SceneEvaluationSnapshot& snapshot,
                                                              dayo::core::ModelId self,
                                                              std::size_t arrayCapacity) const;
};

} // namespace dayo::core::fx
