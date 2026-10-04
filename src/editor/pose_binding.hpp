#pragma once

#include "core/scene.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dayo::editor {

struct BoneEditScratch {
    core::Float3 baseTranslation{};
    core::Float4 baseRotation{0.0F, 0.0F, 0.0F, 1.0F};
    core::Float3 translation{};
    core::Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    bool basePhysics{true};
    bool physics{true};
    bool modified{};
    void revert() noexcept;
    void initialize() noexcept;
};

// Evaluated data is owned here, rather than kept as pointers into frame scratch.
// Authoring scratch resets only when model/frame/motion revision changes.
class PoseBinding {
  public:
    void observe(core::ModelId model, const core::AnimatedModelFrame& frame);
    void synchronize(const core::Scene& scene, float frame);
    void selectBone(int bone, bool add = false, bool toggle = false);
    void setBones(std::vector<int> bones);
    [[nodiscard]] BoneEditScratch* activeEdit() noexcept;
    [[nodiscard]] const std::vector<int>& selectedBones() const noexcept {
        return selectedBones_;
    }
    [[nodiscard]] int activeBone() const noexcept {
        return activeBone_;
    }
    [[nodiscard]] const std::vector<core::AnimatedModelFrame::BoneTransform>* poses(core::ModelId model) const noexcept;
    [[nodiscard]] std::vector<mmd::BoneOverride> overrides(core::ModelId model, float frame,
                                                           std::uint64_t revision) const;
    void registerBones(class EditorSession& session, std::uint32_t frame);
    void translateSelected(core::Float3 delta, const core::ModelInstance& model, bool world);
    void rotateSelected(core::Float4 delta, const core::ModelInstance& model, bool world);
    [[nodiscard]] const BoneEditScratch* edit(int bone) const noexcept;
    void setPhysicsSelected(bool enabled) noexcept;
    void revertSelected() noexcept;
    void initializeSelected() noexcept;
    [[nodiscard]] bool modified() const noexcept;
    [[nodiscard]] core::ModelId target() const noexcept {
        return target_;
    }

  private:
    std::unordered_map<core::ModelId, std::vector<core::AnimatedModelFrame::BoneTransform>> evaluated_;
    std::unordered_map<int, BoneEditScratch> edits_;
    std::vector<int> selectedBones_;
    core::ModelId target_{};
    float frame_{-1.0F};
    std::uint64_t revision_{};
    int activeBone_{-1};
};

} // namespace dayo::editor
