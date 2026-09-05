#pragma once

#include "core/model_probe.hpp"
#include "core/motion.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dayo::core {

class MmdPhysics;

struct AnimatedModelFrame {
    struct BoneTransform {
        Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
        Float3 translation{};
    };
    std::vector<PmxVertex> vertices;
    // Effective vertex-morph weights are populated for GPU skinning. The
    // renderer combines these weights with its immutable sparse morph table.
    std::vector<float> morphWeights;
    std::vector<BoneTransform> bones;
    struct Material {
        Float4 diffuse{};
        Float3 specular{};
        float shininess{};
        Float3 ambient{};
        Float4 edgeColor{};
        float edgeSize{};
        Float4 textureMultiply{1.0F, 1.0F, 1.0F, 1.0F};
        Float4 textureAdd{};
        Float4 sphereMultiply{1.0F, 1.0F, 1.0F, 1.0F};
        Float4 sphereAdd{};
        Float4 toonMultiply{1.0F, 1.0F, 1.0F, 1.0F};
        Float4 toonAdd{};
    };
    std::vector<Material> materials;
    bool visible{true};
};

struct PreviewNormalization {
    Float3 center{};
    float scale{1.0F};
};

struct MotionCompatibility {
    std::size_t pmxBoneCount{};
    std::size_t vmdBoneKeyCount{};
    std::size_t vmdBoneTrackCount{};
    std::size_t matchedBoneKeyCount{};
    std::size_t matchedBoneTrackCount{};
};

class MmdAnimator {
  public:
    explicit MmdAnimator(const PmxModel& model);
    ~MmdAnimator();

    void setMotion(const VmdMotion* motion);
    void setPose(const VpdPose* pose);
    void setPhysics(MmdPhysics* physics);
    [[nodiscard]] MotionCompatibility motionCompatibility() const;
    [[nodiscard]] AnimatedModelFrame evaluate(float frame, float deltaSeconds = 0.0F, bool gpuSkinning = false);

  private:
    struct Impl;
    const PmxModel& model_;
    const VmdMotion* motion_{};
    const VpdPose* pose_{};
    MmdPhysics* physics_{};
    float previousFrame_{-1.0F};
    std::unique_ptr<Impl> impl_;
};

// Applies one stable model-space transform so animated vertices remain framed.
void normalizeForPreview(std::vector<PmxVertex>& vertices, const PreviewNormalization& normalization);
[[nodiscard]] PreviewNormalization previewNormalization(const PmxModel& model);

} // namespace dayo::core
