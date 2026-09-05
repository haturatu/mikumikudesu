#pragma once

#include "core/model_probe.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::core {

struct VmdBoneKey {
    std::string name;
    std::uint32_t frame{};
    Float3 translation{};
    Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    // Canonical control points occupy [axis + point * 4]; remaining bytes are reserved.
    std::array<std::uint8_t, 64> interpolation{};
    bool physics{true};
    std::array<std::uint8_t, 4> methods{};
};

struct VmdMorphKey {
    std::string name;
    std::uint32_t frame{};
    float weight{};
};

struct VmdCameraKey {
    std::uint32_t frame{};
    float distance{};
    Float3 position{};
    Float3 rotation{};
    std::array<std::uint8_t, 24> interpolation{};
    float viewAngle{};
    bool perspective{};
    // VMdayo/.dayo extension; VMD export intentionally omits these fields.
    std::int32_t parentModel{-1};
    std::int32_t parentBone{-1};
    std::string parentBoneName;
    std::array<std::uint8_t, 6> methods{};
};

struct VmdLightKey {
    std::uint32_t frame{};
    Float3 color{};
    Float3 position{};
};
struct VmdShadowKey {
    std::uint32_t frame{};
    std::uint8_t mode{};
    float distance{};
};
struct VmdIkState {
    std::string name;
    bool enabled{};
};
struct VmdIkKey {
    std::uint32_t frame{};
    bool visible{};
    std::vector<VmdIkState> states;
};

struct VmdayoExternalParentKey {
    std::uint32_t frame{};
    std::int32_t parentModel{-1};
    std::string parentBone;
    std::string childBone;
};

struct VmdayoGravityKey {
    std::uint32_t frame{};
    float strength{98.0F};
    Float3 direction{0.0F, -1.0F, 0.0F};
    float noiseAmplitude{};
    float noiseFrequency{};
};

enum class InterpolationMode { linear, bezier, catmullRom };

// Format-neutral motion document shared by VMD, VPD and VMdayo importers.
// Keeping the tracks independent allows camera/light data to coexist with
// model motion in a multi-model Scene.
struct MotionDocument {
    std::string modelName;
    InterpolationMode interpolation{InterpolationMode::bezier};
    std::vector<VmdBoneKey> bones;
    std::vector<VmdMorphKey> morphs;
    std::vector<VmdCameraKey> cameras;
    std::vector<VmdLightKey> lights;
    std::vector<VmdShadowKey> shadows;
    std::vector<VmdIkKey> ik;
    std::vector<VmdayoExternalParentKey> externalParents;
    std::vector<VmdayoGravityKey> gravity;
};

struct VmdMotion {
    std::string modelName;
    std::vector<VmdBoneKey> bones;
    std::vector<VmdMorphKey> morphs;
    std::vector<VmdCameraKey> cameras;
    std::vector<VmdLightKey> lights;
    std::vector<VmdShadowKey> shadows;
    std::vector<VmdIkKey> ik;
    std::vector<VmdayoExternalParentKey> externalParents;
    std::vector<VmdayoGravityKey> gravity;
    std::uint32_t lastFrame{};
    InterpolationMode interpolation{InterpolationMode::bezier};
};

struct VmdCameraState {
    float distance{-45.0F};
    Float3 position{};
    Float3 rotation{};
    float viewAngle{30.0F};
    bool perspective{true};
};

struct VpdBonePose {
    std::string name;
    Float3 translation{};
    Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
};

struct VpdPose {
    std::vector<VpdBonePose> bones;
};

[[nodiscard]] std::string decodeCp932(std::string_view input);
[[nodiscard]] std::string encodeCp932(std::string_view input);
[[nodiscard]] VmdMotion loadVmd(const std::filesystem::path& path);
void saveVmd(const std::filesystem::path& path, const VmdMotion& motion);
[[nodiscard]] VpdPose loadVpd(const std::filesystem::path& path);
[[nodiscard]] MotionDocument toMotionDocument(const VmdMotion& motion);
[[nodiscard]] VmdMotion toVmdMotion(MotionDocument document, std::string modelName = {});
[[nodiscard]] VmdCameraState evaluateCamera(const VmdMotion& motion, float frame);
[[nodiscard]] VmdLightKey evaluateLight(const VmdMotion& motion, float frame);
[[nodiscard]] float catmullRom(float p0, float p1, float p2, float p3, float t) noexcept;

} // namespace dayo::core
