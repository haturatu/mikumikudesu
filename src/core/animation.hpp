#pragma once

#include "core/model_probe.hpp"
#include "core/motion.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dayo::core {

class MmdPhysics;

struct AnimatedModelFrame {
    std::vector<PmxVertex> vertices;
    std::vector<Float4> materialDiffuse;
    bool visible { true };
};

struct PreviewNormalization {
    Float3 center {};
    float scale { 1.0F };
};

class MmdAnimator {
public:
    explicit MmdAnimator(const PmxModel& model);

    void setMotion(const VmdMotion* motion);
    void setPose(const VpdPose* pose);
    void setPhysics(MmdPhysics* physics);
    [[nodiscard]] AnimatedModelFrame evaluate(float frame, float deltaSeconds = 0.0F);

private:
    const PmxModel& model_;
    const VmdMotion* motion_ {};
    const VpdPose* pose_ {};
    MmdPhysics* physics_ {};
    float previousFrame_ { -1.0F };
};

// Applies one stable model-space transform so animated vertices remain framed.
void normalizeForPreview(std::vector<PmxVertex>& vertices, const PmxModel& model);
[[nodiscard]] PreviewNormalization previewNormalization(const PmxModel& model);

} // namespace dayo::core
