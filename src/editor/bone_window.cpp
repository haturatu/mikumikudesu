#include "editor/bone_window.hpp"

#include "editor/editor_session.hpp"

namespace dayo::editor {

void BoneWindow::commitAsNewKey(EditorSession& session, std::uint32_t frame) {
    auto* scene = session.scene();
    if (scene == nullptr || bone_.empty())
        return;
    const auto* current = scene->motion(session.target(), session.global());
    core::VmdMotion next = current != nullptr ? *current : core::VmdMotion{};
    core::VmdBoneKey key;
    key.name = bone_;
    key.frame = frame;
    key.translation = translation_;
    key.rotation = rotation_;
    next.bones.push_back(std::move(key));
    next.lastFrame = std::max(next.lastFrame, frame);
    ReplaceMotionOperation operation;
    operation.target = session.target();
    operation.global = session.global();
    operation.motion = std::move(next);
    operation.label = "Add bone key";
    session.operations().push(std::move(operation));
}

} // namespace dayo::editor
