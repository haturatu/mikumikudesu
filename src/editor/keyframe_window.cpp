#include "editor/keyframe_window.hpp"

#include "editor/editor_session.hpp"

namespace dayo::editor {

void KeyframeWindow::refresh(EditorSession& session) {
    rows_.clear();
    const auto* scene = session.scene();
    if (scene == nullptr)
        return;
    const auto* motion = scene->motion(session.target(), session.global());
    if (motion == nullptr)
        return;
    auto pushBone = [&](const auto& key, std::int32_t track, std::uint64_t id) {
        Row row;
        row.stableId = id;
        row.track = track;
        row.name = key.name;
        row.frame = key.frame;
        row.selected = session.selection().contains(MotionKeyId{static_cast<core::MotionTrack>(track), id});
        rows_.push_back(std::move(row));
    };
    std::uint64_t id = 1;
    for (const auto& key : motion->bones)
        pushBone(key, 0, id++);
    for (const auto& key : motion->morphs)
        pushBone(key, 1, id++);
    // Camera/light/shadow/IK rows are summarized by frame only; saving
    // re-sorts by frame so the list order here is presentational.
}

void KeyframeWindow::requestMoveSelected(EditorSession& session, std::int64_t frameDelta) {
    if (frameDelta == 0 || session.selection().empty())
        return;
    MoveKeysOperation operation;
    operation.target = session.target();
    operation.global = session.global();
    operation.frameDelta = frameDelta;
    for (const auto& id : session.selection().ids())
        operation.stableIds.push_back(id.stableId);
    session.operations().push(std::move(operation));
}

} // namespace dayo::editor
