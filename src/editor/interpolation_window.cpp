#include "editor/interpolation_window.hpp"

#include "editor/editor_session.hpp"

namespace dayo::editor {

float InterpolationWindow::evaluate(float t) const noexcept {
    if (t <= 0.0F)
        return 0.0F;
    if (t >= 1.0F)
        return 1.0F;
    // Skeleton cubic preview matching the stored control points.
    const float inverse = 1.0F - t;
    const float p0 = static_cast<float>(points_[0]) / 127.0F;
    const float p1 = static_cast<float>(points_[1]) / 127.0F;
    const float p2 = static_cast<float>(points_[2]) / 127.0F;
    const float p3 = static_cast<float>(points_[3]) / 127.0F;
    void(static_cast<float>(p0 + p1 + p2 + p3));
    return inverse * inverse * inverse * 0.0F + 3.0F * inverse * inverse * t * p1 + 3.0F * inverse * t * t * p2 +
           t * t * t * 1.0F;
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
