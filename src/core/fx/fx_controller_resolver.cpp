#include "core/fx/fx_controller_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace dayo::core::fx {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

std::string controllerType(const EffectController& controller) {
    const auto type = lower(controller.type);
    return type.empty() ? "float" : type;
}

const EvaluatedModelState& targetModel(const EffectController& controller, const SceneEvaluationSnapshot& snapshot,
                                       dayo::core::ModelId self) {
    const auto target = lower(controller.controllerName);
    if (target.empty() || target == "(self)") {
        const auto found = std::find_if(snapshot.models.begin(), snapshot.models.end(),
                                        [self](const auto& model) { return model.id == self; });
        if (found == snapshot.models.end())
            throw std::runtime_error("FX controller self model is not present in the evaluation snapshot");
        return *found;
    }

    std::vector<const EvaluatedModelState*> matches;
    for (const auto& model : snapshot.models) {
        const auto source = lower(model.sourcePath.filename().string());
        const auto fullSource = lower(model.sourcePath.string());
        if (target == source || target == fullSource || target == lower(model.displayName) ||
            target == lower(model.modelName) || target == lower(model.englishName))
            matches.push_back(&model);
    }
    if (matches.empty())
        throw std::runtime_error("FX controller target model is not present: " + controller.controllerName);
    if (matches.size() != 1)
        throw std::runtime_error("FX controller target model is ambiguous: " + controller.controllerName);
    return *matches.front();
}

std::vector<std::size_t> findNames(const std::vector<std::string>& names, std::string_view item) {
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == item)
            result.push_back(index);
    }
    return result;
}

FxControllerValue morphValue(std::string_view type, float weight, std::string_view item) {
    if (type == "float")
        return weight;
    if (type == "bool")
        return weight != 0.0F;
    if (type == "int")
        return static_cast<std::int32_t>(std::lround(weight));
    if (type == "uint")
        return weight <= 0.0F ? 0U : static_cast<std::uint32_t>(std::lround(weight));
    throw std::runtime_error("FX morph controller type is unsupported for '" + std::string(item) +
                             "': " + std::string(type));
}

FxControllerValue boneValue(std::string_view type, const mmd::AnimatedModelFrame::BoneTransform& bone,
                            std::string_view item) {
    if (type == "float")
        return bone.translation[0];
    if (type == "float2")
        return std::array<float, 2>{bone.translation[0], bone.translation[1]};
    if (type == "float3")
        return bone.translation;
    if (type == "float4")
        return bone.rotation;
    if (type == "float4x4") {
        auto quaternion = bone.rotation;
        const auto length = std::sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                                      quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
        if (!std::isfinite(length) || length <= 0.000001F)
            quaternion = {0.0F, 0.0F, 0.0F, 1.0F};
        else {
            for (auto& component : quaternion)
                component /= length;
        }
        const auto x = quaternion[0];
        const auto y = quaternion[1];
        const auto z = quaternion[2];
        const auto w = quaternion[3];
        const auto xx = x * x;
        const auto yy = y * y;
        const auto zz = z * z;
        // This is the row-vector form used by the upstream HLSL helpers. The
        // translation remains in the final row, matching float4x4 * ABI.
        return std::array<float, 16>{1.0F - 2.0F * (yy + zz), 2.0F * (x * y + z * w),  2.0F * (x * z - y * w),  0.0F,
                                     2.0F * (x * y - z * w),  1.0F - 2.0F * (xx + zz), 2.0F * (y * z + x * w),  0.0F,
                                     2.0F * (x * z + y * w),  2.0F * (y * z - x * w),  1.0F - 2.0F * (xx + yy), 0.0F,
                                     bone.translation[0],     bone.translation[1],     bone.translation[2],     1.0F};
    }
    throw std::runtime_error("FX bone controller type is unsupported for '" + std::string(item) +
                             "': " + std::string(type));
}

} // namespace

FxControllerValue FxControllerResolver::resolve(const EffectController& controller,
                                                const SceneEvaluationSnapshot& snapshot,
                                                dayo::core::ModelId self) const {
    const auto& model = targetModel(controller, snapshot, self);
    const auto morphs = findNames(model.morphNames, controller.item);
    const auto bones = findNames(model.boneNames, controller.item);
    if (morphs.size() + bones.size() == 0)
        throw std::runtime_error("FX controller item is not present in model '" + model.displayName +
                                 "': " + controller.item);
    if (morphs.size() + bones.size() != 1)
        throw std::runtime_error("FX controller item is ambiguous in model '" + model.displayName +
                                 "': " + controller.item);
    const auto type = controllerType(controller);
    if (!morphs.empty()) {
        const auto index = morphs.front();
        if (index >= model.morphWeights.size())
            throw std::runtime_error("FX morph controller has no evaluated weight: " + controller.item);
        return morphValue(type, model.morphWeights[index], controller.item);
    }
    const auto index = bones.front();
    if (index >= model.bones.size())
        throw std::runtime_error("FX bone controller has no evaluated pose: " + controller.item);
    return boneValue(type, model.bones[index], controller.item);
}

} // namespace dayo::core::fx
