#pragma once

#include "core/animation.hpp"
#include "graphics/device.hpp"

#include <algorithm>
#include <cstring>

namespace dayo::app {

inline core::Float3 rotateQuaternion(const core::Float4& quaternion, const core::Float3& value) {
    const core::Float3 axis{quaternion[0], quaternion[1], quaternion[2]};
    const core::Float3 firstCross{
        axis[1] * value[2] - axis[2] * value[1],
        axis[2] * value[0] - axis[0] * value[2],
        axis[0] * value[1] - axis[1] * value[0],
    };
    const core::Float3 nested{
        axis[1] * firstCross[2] - axis[2] * firstCross[1],
        axis[2] * firstCross[0] - axis[0] * firstCross[2],
        axis[0] * firstCross[1] - axis[1] * firstCross[0],
    };
    return {
        value[0] + 2.0F * (nested[0] + quaternion[3] * firstCross[0]),
        value[1] + 2.0F * (nested[1] + quaternion[3] * firstCross[1]),
        value[2] + 2.0F * (nested[2] + quaternion[3] * firstCross[2]),
    };
}

inline core::Float3 normalizePreviewPoint(const core::Float3& point, const core::PreviewNormalization& normalization) {
    return {
        (point[0] - normalization.center[0]) * normalization.scale,
        (point[1] - normalization.center[1]) * normalization.scale,
        (point[2] - normalization.center[2]) * normalization.scale,
    };
}

inline graphics::PreviewBoneTransform makePreviewBone(const core::AnimatedModelFrame::BoneTransform& source,
                                                      const core::PreviewNormalization& normalization) {
    graphics::PreviewBoneTransform bone;
    std::copy(source.rotation.begin(), source.rotation.end(), bone.rotation);
    const auto rotatedCenter = rotateQuaternion(source.rotation, normalization.center);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        bone.translation[axis] =
            (rotatedCenter[axis] + source.translation[axis] - normalization.center[axis]) * normalization.scale;
    }
    return bone;
}

inline graphics::PreviewVertex makePreviewVertex(const core::PmxVertex& source, std::size_t boneCount,
                                                 std::int32_t boneOffset,
                                                 const core::PreviewNormalization& normalization,
                                                 std::array<std::uint32_t, 2> morphRange, bool gpuSkinning) {
    graphics::PreviewVertex vertex;
    std::memcpy(vertex.position, source.position.data(), sizeof(vertex.position));
    std::memcpy(vertex.normal, source.normal.data(), sizeof(vertex.normal));
    std::memcpy(vertex.uv, source.uv.data(), sizeof(vertex.uv));
    if (gpuSkinning) {
        for (std::size_t influence = 0; influence < 4; ++influence) {
            vertex.bones[influence] =
                source.bones[influence] < 0 || static_cast<std::size_t>(source.bones[influence]) >= boneCount
                    ? -1
                    : boneOffset + source.bones[influence];
            vertex.weights[influence] = source.weights[influence];
        }
        const auto normalizedC = normalizePreviewPoint(source.sdefC, normalization);
        const auto normalizedR0 = normalizePreviewPoint(source.sdefR0, normalization);
        const auto normalizedR1 = normalizePreviewPoint(source.sdefR1, normalization);
        std::copy(normalizedC.begin(), normalizedC.end(), vertex.sdefC);
        for (std::size_t axis = 0; axis < 3; ++axis)
            vertex.sdefHalfDelta[axis] = (normalizedR0[axis] - normalizedR1[axis]) * 0.5F;
        vertex.skinningType = static_cast<std::uint32_t>(source.weightType);
        vertex.gpuSkinning = 1;
    }
    vertex.edgeScale = source.edgeScale;
    vertex.morphStart = morphRange[0];
    vertex.morphCount = gpuSkinning ? morphRange[1] : 0U;
    return vertex;
}

} // namespace dayo::app
