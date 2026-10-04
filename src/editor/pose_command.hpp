#pragma once
#include "core/editor.hpp"
#include "editor/motion_key_id.hpp"
namespace dayo::editor {
// Registering a VPD-derived pose removes only the corresponding persistent
// VPD overrides, so they cannot shadow the newly authored VMD keys.
class RegisterPoseCommand final : public core::EditCommand {
  public:
    RegisterPoseCommand(const core::ModelInstance& model, core::VmdMotion after, std::vector<std::string> bones,
                        StableIdTable* ids = nullptr);
    void apply(core::Scene& scene) override;
    void undo(core::Scene& scene) override;
    const char* name() const noexcept override {
        return "Register bone keys";
    }

  private:
    void setPose(core::Scene& scene, const std::optional<core::VpdPose>& pose);
    StableIdTable* ids_{};
    StableIdTable beforeIds_, afterIds_;
    core::ModelId target_{};
    core::EditMotionCommand motion_;
    std::optional<core::VpdPose> before_, after_;
};
} // namespace dayo::editor
