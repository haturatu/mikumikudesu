#pragma once

#include "core/model_probe.hpp"
#include "core/motion.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dayo::core {

struct AnimatedModelFrame {
    std::vector<PmxVertex> vertices;
    std::vector<Float4> materialDiffuse;
    bool visible { true };
};

class MmdAnimator {
public:
    explicit MmdAnimator(const PmxModel& model);

    void setMotion(const VmdMotion* motion);
    void setPose(const VpdPose* pose);
    [[nodiscard]] AnimatedModelFrame evaluate(float frame) const;

private:
    const PmxModel& model_;
    const VmdMotion* motion_ {};
    const VpdPose* pose_ {};
};

// Applies one stable model-space transform so animated vertices remain framed.
void normalizeForPreview(std::vector<PmxVertex>& vertices, const PmxModel& model);

} // namespace dayo::core
