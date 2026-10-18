#include "editor/interpolation_window.hpp"

#include "editor/editor_session.hpp"
#include "editor/interpolation_bezier.hpp"

#include <utility>
#include <vector>

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

    // Resolve stable identities against the unmodified motion snapshot.
    // Never assume that an index persisted across sorting or key insertion.
    const auto document = core::toMotionDocument(*current);
    const MotionKeyId target{core::MotionTrack::bone, stableId_};
    std::vector<MotionKeyId> targets;
    if (session.selection().contains(target)) {
        for (const auto& id : session.selection().ids())
            if (id.track == core::MotionTrack::bone)
                targets.push_back(id);
    } else {
        targets.push_back(target);
    }

    core::VmdMotion next = *current;
    bool changed = false;
    for (const auto& id : targets) {
        const auto index = session.stableIds().resolve(document, id);
        if (!index.has_value() || *index >= next.bones.size())
            continue;
        auto& key = next.bones[*index];
        // Bone VMD interpolation is channel-major within each point:
        // [x1, y1, x2, y2] at [axis, axis+4, axis+8, axis+12].
        for (std::size_t point = 0; point < points_.size(); ++point) {
            auto& encoded = key.interpolation[axis_ + point * 4U];
            if (encoded != points_[point]) {
                encoded = points_[point];
                changed = true;
            }
        }
        if (key.methods[axis_] != method_) {
            key.methods[axis_] = method_;
            changed = true;
        }
    }
    if (!changed)
        return;

    ReplaceMotionOperation operation;
    operation.target = session.target();
    operation.global = session.global();
    operation.motion = std::move(next);
    operation.label = "Edit interpolation";
    session.operations().push(std::move(operation));
}

} // namespace dayo::editor
