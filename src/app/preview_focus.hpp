#pragma once

#include "app/preview_conversion.hpp"
#include "core/fx/fx_controller_resolver.hpp"
#include "core/project.hpp"
#include <limits>

#include <cctype>
#include <span>
#include <string_view>

namespace dayo::app {

constexpr auto previewMissingFocusModel = std::numeric_limits<std::uint32_t>::max();

inline bool samePreviewFocusSource(const std::filesystem::path& left, const std::filesystem::path& right) {
    return !left.empty() && !right.empty() &&
           std::filesystem::absolute(left).lexically_normal() == std::filesystem::absolute(right).lexically_normal();
}

// Native project metadata order can differ from asset loading order. Map through
// the project's existing source reference, including missing model entries.
inline std::uint32_t previewFocusProjectIndex(std::span<const core::ModelInstance> models,
                                              std::span<const core::ProjectModelState> projectModels,
                                              std::uint32_t sceneIndex) {
    if (sceneIndex >= models.size())
        return previewMissingFocusModel;
    for (std::size_t i = 0; i < projectModels.size(); ++i)
        if (samePreviewFocusSource(models[sceneIndex].sourcePath, projectModels[i].source))
            return static_cast<std::uint32_t>(i);
    return previewMissingFocusModel;
}

inline std::uint32_t previewFocusSceneIndex(std::span<const core::ModelInstance> models,
                                            std::span<const core::ProjectModelState> projectModels,
                                            std::uint32_t projectIndex) {
    if (projectIndex >= projectModels.size())
        return previewMissingFocusModel;
    if (projectModels[projectIndex].source.empty())
        return projectIndex < models.size() ? projectIndex : previewMissingFocusModel;
    for (std::size_t i = 0; i < models.size(); ++i)
        if (samePreviewFocusSource(models[i].sourcePath, projectModels[projectIndex].source))
            return static_cast<std::uint32_t>(i);
    return previewMissingFocusModel;
}

inline std::string canonicalFocusBoneName(std::string_view name) {
    std::string result;
    for (const char rawLetter : name) {
        const auto letter = static_cast<unsigned char>(rawLetter);
        if (letter != ' ' && letter != '_' && letter != '-')
            result += static_cast<char>(letter < 128 ? std::tolower(letter) : letter);
    }
    return result;
}

// Resolve against the same evaluated skinning transforms used by the mesh.
// A BoneTransform rotates a bind-space point; its translation alone is not
// the animated bone origin. Model order and bone name survive project reload.
inline std::optional<core::Float3> previewFocusBonePoint(const core::ModelInstance& instance,
                                                         const core::fx::SceneEvaluationSnapshot& snapshot,
                                                         const graphics::PreviewDofSettings& settings) {
    if (instance.model == nullptr)
        return std::nullopt;
    const auto evaluated = std::ranges::find(snapshot.models, instance.id, &core::fx::EvaluatedModelState::id);
    const auto point = [&](std::span<const std::string_view> names,
                           bool faceAnchor = false) -> std::optional<core::Float3> {
        for (const auto name : names) {
            const auto canonical = canonicalFocusBoneName(name);
            const auto& bones = instance.model->bones;
            for (std::size_t i = 0; i < bones.size(); ++i) {
                if (canonicalFocusBoneName(bones[i].name) != canonical &&
                    canonicalFocusBoneName(bones[i].englishName) != canonical)
                    continue;
                auto position = bones[i].position;
                if (evaluated != snapshot.models.end() && i < evaluated->bones.size()) {
                    const auto& transform = evaluated->bones[i];
                    position = rotateQuaternion(transform.rotation, position);
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        position[axis] += transform.translation[axis];
                }
                auto result = normalizePreviewPoint(position, instance.normalization);
                core::Float3 offset = settings.localOffsetMetres;
                if (faceAnchor)
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        offset[axis] += settings.faceOffsetMetres[axis];
                for (auto& value : offset)
                    value /= settings.metresPerUnit;
                if (evaluated != snapshot.models.end() && i < evaluated->bones.size())
                    offset = rotateQuaternion(evaluated->bones[i].rotation, offset);
                for (std::size_t axis = 0; axis < 3; ++axis)
                    result[axis] += offset[axis];
                if (settings.focusMode == graphics::PreviewFocusMode::trackedSurface) {
                    if (settings.trackingInstance >= instance.cloneCount)
                        return std::nullopt;
                    result[0] += (static_cast<float>(settings.trackingInstance) -
                                  (static_cast<float>(instance.cloneCount) - 1.0F) * 0.5F) *
                                 2.2F;
                }
                return result;
            }
        }
        return std::nullopt;
    };
    if (settings.focusMode == graphics::PreviewFocusMode::bone ||
        settings.focusMode == graphics::PreviewFocusMode::trackedSurface) {
        const std::array<std::string_view, 1> names{settings.boneName};
        if (settings.boneName.empty())
            return std::nullopt;
        return point(names);
    }
    if (settings.focusMode == graphics::PreviewFocusMode::face) {
        const std::array<std::string_view, 2> headNames{"頭", "head"};
        if (const auto face = point(headNames, true))
            return face;
    }
    const std::array<std::string_view, 4> leftNames{"左目", "left eye", "eye_l", "eye.l"};
    const std::array<std::string_view, 4> rightNames{"右目", "right eye", "eye_r", "eye.r"};
    const auto left = point(leftNames);
    const auto right = point(rightNames);
    if (left && right)
        return core::Float3{((*left)[0] + (*right)[0]) * 0.5F, ((*left)[1] + (*right)[1]) * 0.5F,
                            ((*left)[2] + (*right)[2]) * 0.5F};
    if (left)
        return left;
    if (right)
        return right;
    const std::array<std::string_view, 3> eyeNames{"両目", "eyes", "eye"};
    if (const auto eyes = point(eyeNames))
        return eyes;
    const std::array<std::string_view, 2> headNames{"頭", "head"};
    return point(headNames);
}

struct PreviewFocusModelBinding {
    std::shared_ptr<const core::PmxModel> model;
    core::PreviewNormalization normalization;
    core::ModelId id{};
    std::uint32_t materialFirst{};
    std::uint32_t materialCount{};
    std::uint32_t boneFirst{};
    bool gpuSkinning{};
};

struct PreviewTrackedFocus {
    std::string boneName;
    core::Float3 localOffsetMetres{};
    core::ModelId model{};
    std::uint32_t instance{};
};

inline std::optional<PreviewTrackedFocus> previewTrackedFocus(const graphics::PreviewFocusSample& sample,
                                                              std::span<const PreviewFocusModelBinding> bindings,
                                                              const core::fx::SceneEvaluationSnapshot& fallback,
                                                              float metresPerUnit) {
    const auto binding = std::ranges::find_if(bindings, [&](const auto& value) {
        return sample.materialIndex >= value.materialFirst &&
               sample.materialIndex - value.materialFirst < value.materialCount;
    });
    if (binding == bindings.end() || !binding->model || sample.instanceIndex >= sample.instanceCount)
        return std::nullopt;
    const auto evaluated = std::ranges::find(fallback.models, binding->id, &core::fx::EvaluatedModelState::id);
    float bestDistance = std::numeric_limits<float>::infinity();
    std::optional<PreviewTrackedFocus> result;
    const float cloneOffset =
        (static_cast<float>(sample.instanceIndex) - (static_cast<float>(sample.instanceCount) - 1.0F) * 0.5F) * 2.2F;
    for (std::size_t i = 0; i < binding->model->bones.size(); ++i) {
        auto anchor = normalizePreviewPoint(binding->model->bones[i].position, binding->normalization);
        core::Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
        if (binding->gpuSkinning && binding->boneFirst + i < sample.bones.size()) {
            const auto& bone = sample.bones[binding->boneFirst + i];
            std::copy_n(bone.rotation, 4, rotation.begin());
            anchor = rotateQuaternion(rotation, anchor);
            for (std::size_t axis = 0; axis < 3; ++axis)
                anchor[axis] += bone.translation[axis];
        } else if (evaluated != fallback.models.end() && i < evaluated->bones.size()) {
            const auto& bone = evaluated->bones[i];
            rotation = bone.rotation;
            auto point = rotateQuaternion(rotation, binding->model->bones[i].position);
            for (std::size_t axis = 0; axis < 3; ++axis)
                point[axis] += bone.translation[axis];
            anchor = normalizePreviewPoint(point, binding->normalization);
        } else {
            continue;
        }
        anchor[0] += cloneOffset;
        core::Float3 offset{};
        float distance = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            offset[axis] = sample.worldPoint[axis] - anchor[axis];
            distance += offset[axis] * offset[axis];
        }
        if (distance >= bestDistance || !std::isfinite(distance))
            continue;
        bestDistance = distance;
        const core::Float4 inverse{-rotation[0], -rotation[1], -rotation[2], rotation[3]};
        offset = rotateQuaternion(inverse, offset);
        for (auto& value : offset)
            value *= metresPerUnit;
        result = PreviewTrackedFocus{binding->model->bones[i].name, offset, binding->id, sample.instanceIndex};
    }
    return result;
}

inline float resolvePreviewFocusDistance(const graphics::PreviewDofSettings& settings,
                                         const graphics::PreviewFocusCamera& camera,
                                         std::span<const core::ModelInstance> models,
                                         const core::fx::SceneEvaluationSnapshot& snapshot) {
    using graphics::PreviewFocusMode;
    if (settings.focusMode == PreviewFocusMode::manual)
        return settings.focusDistanceMetres / settings.metresPerUnit;
    if (settings.focusMode == PreviewFocusMode::viewportPoint)
        return std::max(0.05F, graphics::previewFocusViewPoint(camera, settings.worldPoint)[2]);
    if (settings.focusMode != PreviewFocusMode::cameraTarget && settings.modelIndex < models.size()) {
        if (const auto point = previewFocusBonePoint(models[settings.modelIndex], snapshot, settings))
            return std::max(0.05F, graphics::previewFocusViewPoint(camera, *point)[2]);
    }
    return std::max(std::abs(camera.distance), 0.1F);
}

} // namespace dayo::app
