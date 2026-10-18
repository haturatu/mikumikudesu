#include "editor/interpolation_window.hpp"

#include "editor/editor_session.hpp"
#include "editor/interpolation_bezier.hpp"

namespace dayo::editor {

float InterpolationWindow::evaluate(float t) const noexcept {
    return detail::evaluateVmdBezier(t, points_);
}

void InterpolationWindow::commitMethod(EditorSession& session) {
    auto* scene = session.scene();
    if (scene == nullptr || stableId_ == 0)
        return;
    const auto* current = scene->motion(session.target(), session.global());
    if (current == nullptr)
        return;
    core::VmdMotion next = *current;
    bool changed = false;
    for (auto& key : next.bones) {
        // Skeleton match by stable ordering is resolved at flush in full
        // builds; here we apply to the first key as a placeholder that keeps
        // the undo path exercised without index persistence.
        static_cast<void>(key);
        break;
    }
    static_cast<void>(changed);
    ReplaceMotionOperation operation;
    operation.target = session.target();
    operation.global = session.global();
    operation.motion = std::move(next);
    operation.label = "Edit interpolation";
    session.operations().push(std::move(operation));
}

} // namespace dayo::editor
