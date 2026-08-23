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
    std::uint32_t frame {};
    Float3 translation {};
    Float4 rotation { 0.0F, 0.0F, 0.0F, 1.0F };
    std::array<std::uint8_t, 64> interpolation {};
};

struct VmdMorphKey { std::string name; std::uint32_t frame {}; float weight {}; };

struct VmdCameraKey {
    std::uint32_t frame {};
    float distance {};
    Float3 position {};
    Float3 rotation {};
    std::array<std::uint8_t, 24> interpolation {};
    std::uint32_t viewAngle {};
    bool perspective {};
};

struct VmdLightKey { std::uint32_t frame {}; Float3 color {}; Float3 position {}; };
struct VmdShadowKey { std::uint32_t frame {}; std::uint8_t mode {}; float distance {}; };
struct VmdIkState { std::string name; bool enabled {}; };
struct VmdIkKey { std::uint32_t frame {}; bool visible {}; std::vector<VmdIkState> states; };

struct VmdMotion {
    std::string modelName;
    std::vector<VmdBoneKey> bones;
    std::vector<VmdMorphKey> morphs;
    std::vector<VmdCameraKey> cameras;
    std::vector<VmdLightKey> lights;
    std::vector<VmdShadowKey> shadows;
    std::vector<VmdIkKey> ik;
    std::uint32_t lastFrame {};
};

struct VpdBonePose {
    std::string name;
    Float3 translation {};
    Float4 rotation { 0.0F, 0.0F, 0.0F, 1.0F };
};

struct VpdPose { std::vector<VpdBonePose> bones; };

[[nodiscard]] std::string decodeCp932(std::string_view input);
[[nodiscard]] VmdMotion loadVmd(const std::filesystem::path& path);
[[nodiscard]] VpdPose loadVpd(const std::filesystem::path& path);

} // namespace dayo::core
