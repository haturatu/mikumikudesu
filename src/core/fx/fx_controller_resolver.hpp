#pragma once

#include "core/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
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

// Resolves EffectController declarations against a frame-stable PMX
// evaluation snapshot. It never reaches into a live animator, which keeps
// controller values coherent with the geometry evaluated for the same frame.
class FxControllerResolver {
  public:
    [[nodiscard]] FxControllerValue resolve(const EffectController& controller,
                                            const SceneEvaluationSnapshot& snapshot,
                                            dayo::core::ModelId self) const;
};

} // namespace dayo::core::fx
