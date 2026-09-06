#include "editor/keyframe_window.hpp"

#include "editor/editor_session.hpp"

#include <utility>

namespace dayo::editor {

void KeyframeWindow::refresh(EditorSession& session) {
    rows_.clear();
    const auto* scene = session.scene();
    if (scene == nullptr)
        return;
    const auto* motion = scene->motion(session.target(), session.global());
    if (motion == nullptr)
        return;
    const auto document = core::toMotionDocument(*motion);
    session.stableIds().rebuild(document);
    auto pushRow = [&](std::string name, std::uint32_t frame, core::MotionTrack track, std::size_t index) {
        const auto id = session.stableIds().keyId(track, index);
        Row row;
        row.stableId = id.stableId;
        row.track = static_cast<std::int32_t>(track);
        row.name = std::move(name);
        row.frame = frame;
        row.selected = session.selection().contains(id);
        rows_.push_back(std::move(row));
    };
    for (std::size_t index = 0; index < motion->bones.size(); ++index)
        pushRow(motion->bones[index].name, motion->bones[index].frame, core::MotionTrack::bone, index);
    for (std::size_t index = 0; index < motion->morphs.size(); ++index)
        pushRow(motion->morphs[index].name, motion->morphs[index].frame, core::MotionTrack::morph, index);
    for (std::size_t index = 0; index < motion->cameras.size(); ++index)
        pushRow("camera", motion->cameras[index].frame, core::MotionTrack::camera, index);
    for (std::size_t index = 0; index < motion->lights.size(); ++index)
        pushRow("light", motion->lights[index].frame, core::MotionTrack::light, index);
    for (std::size_t index = 0; index < motion->shadows.size(); ++index)
        pushRow("shadow", motion->shadows[index].frame, core::MotionTrack::shadow, index);
    for (std::size_t index = 0; index < motion->ik.size(); ++index)
        pushRow("ik", motion->ik[index].frame, core::MotionTrack::ik, index);
}

void KeyframeWindow::requestMoveSelected(EditorSession& session, std::int64_t frameDelta) {
    if (frameDelta == 0 || session.selection().empty())
        return;
    MoveKeysOperation operation;
    operation.target = session.target();
    operation.global = session.global();
    operation.frameDelta = frameDelta;
    operation.keys = session.selection().ids();
    session.operations().push(std::move(operation));
}

} // namespace dayo::editor
