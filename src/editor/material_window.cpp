#include "editor/material_window.hpp"

#include "editor/editor_session.hpp"

namespace dayo::editor {

void MaterialWindow::queueMaterialEdit(EditorSession& session) {
    if (entries_.empty())
        return;
    // Skeleton: material writes are not yet modeled as EditCommands, so we
    // enqueue a no-op motion operation to keep the UI -> operation -> history
    // invariant visible while the material command lands.
    auto* scene = session.scene();
    if (scene == nullptr)
        return;
    const auto* current = scene->motion(session.target(), session.global());
    ReplaceMotionOperation operation;
    operation.target = session.target();
    operation.global = session.global();
    operation.motion = current != nullptr ? *current : core::VmdMotion{};
    operation.label = "Edit material (skeleton)";
    session.operations().push(std::move(operation));
}

} // namespace dayo::editor
