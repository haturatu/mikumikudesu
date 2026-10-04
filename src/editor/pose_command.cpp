#include "editor/pose_command.hpp"
#include <algorithm>
namespace dayo::editor {
RegisterPoseCommand::RegisterPoseCommand(const core::ModelInstance& model, core::VmdMotion after,
                                         std::vector<std::string> bones, StableIdTable* ids)
    : ids_(ids), target_(model.id),
      motion_(model.id, false, model.motion ? *model.motion : core::VmdMotion{}, after, "Register bone keys") {
    if (ids_) {
        ids_->rebuild(core::toMotionDocument(model.motion ? *model.motion : core::VmdMotion{}));
        beforeIds_ = *ids_;
        afterIds_ = beforeIds_;
        afterIds_.rebuild(core::toMotionDocument(after));
    }
    if (model.pose) {
        before_ = *model.pose;
        after_ = before_;
        std::erase_if(after_->bones,
                      [&](const auto& bone) { return std::ranges::find(bones, bone.name) != bones.end(); });
        if (after_->bones.empty())
            after_.reset();
    }
}
void RegisterPoseCommand::setPose(core::Scene& scene, const std::optional<core::VpdPose>& pose) {
    auto* model = scene.model(target_);
    if (!model)
        return;
    model->pose = pose ? std::make_unique<core::VpdPose>(*pose) : nullptr;
    model->animator->setPose(model->pose.get());
}
void RegisterPoseCommand::apply(core::Scene& scene) {
    setPose(scene, after_);
    motion_.apply(scene);
    if (ids_)
        *ids_ = afterIds_;
}
void RegisterPoseCommand::undo(core::Scene& scene) {
    setPose(scene, before_);
    motion_.undo(scene);
    if (ids_)
        *ids_ = beforeIds_;
}
} // namespace dayo::editor
