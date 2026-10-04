#pragma once
#include "core/editor.hpp"
namespace dayo::editor {
// Registering a VPD-derived pose removes only the corresponding persistent
// VPD overrides, so they cannot shadow the newly authored VMD keys.
class RegisterPoseCommand final : public core::EditCommand {
  public:
    RegisterPoseCommand(const core::ModelInstance& model, core::VmdMotion after, std::vector<std::string> bones);
    void apply(core::Scene& scene) override;
    void undo(core::Scene& scene) override;
    const char* name() const noexcept override {
        return "Register bone keys";
    }

  private:
    void setPose(core::Scene& scene, const std::optional<core::VpdPose>& pose);
    core::ModelId target_{};
    core::EditMotionCommand motion_;
    std::optional<core::VpdPose> before_, after_;
};
} // namespace dayo::editor
