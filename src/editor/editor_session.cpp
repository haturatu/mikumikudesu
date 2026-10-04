#include "editor/editor_session.hpp"

namespace dayo::editor {

StableIdTable& EditorSession::stableIds() {
    return stableIds(target_, global_);
}
StableIdTable& EditorSession::stableIds(core::ModelId target, bool global) {
    if (global)
        return globalStableIds_;
    auto& table = modelStableIds_[target];
    if (!table)
        table = std::make_unique<StableIdTable>();
    return *table;
}

void EditorSession::setTarget(core::ModelId target, bool global) {
    if (target_ != target || global_ != global) {
        cancelKeyframeDrag();
        selection_.clear();
    }
    target_ = target;
    global_ = global;
}

void EditorSession::beginKeyframeDrag(const std::string& label) {
    if (scene_ == nullptr || history_ == nullptr || transaction_ != nullptr)
        return;
    const auto* motion = scene_->motion(target_, global_);
    if (!motion)
        return;
    dragOriginal_ = *motion;
    dragOriginalIds_ = stableIds();
    dragKeys_ = selection_.ids();
    dragDelta_ = 0;
    dragChanged_ = false;
    transaction_ = std::make_unique<UndoTransaction>(*scene_, *history_, target_, global_, label, &stableIds());
}

void EditorSession::moveKeyframeDrag(std::int64_t delta) {
    if (!transaction_ || !dragOriginal_ || delta == dragDelta_)
        return;
    auto document = core::toMotionDocument(*dragOriginal_);
    std::vector<core::MotionKeyRef> refs;
    for (const auto id : dragKeys_) {
        const auto index = dragOriginalIds_.resolve(document, id);
        if (index)
            refs.push_back({id.track, *index});
    }
    core::MotionEditor::move(document, refs, delta);
    auto ids = dragOriginalIds_;
    ids.notifyMoved(document, dragKeys_, delta);
    ids.rebuild(document);
    stableIds() = std::move(ids);
    updateKeyframeDrag(core::toVmdMotion(std::move(document), dragOriginal_->modelName));
    dragDelta_ = delta;
    dragChanged_ = delta != 0;
}

void EditorSession::updateKeyframeDrag(core::VmdMotion intermediate) {
    if (transaction_ != nullptr) {
        transaction_->dragTo(std::move(intermediate));
        dragChanged_ = true;
    }
}

void EditorSession::commitKeyframeDrag() {
    if (transaction_ != nullptr) {
        if (!dragChanged_)
            transaction_->rollback();
        else
            transaction_->commit();
        transaction_.reset();
        dragOriginal_.reset();
        dragKeys_.clear();
    }
}

void EditorSession::cancelKeyframeDrag() noexcept {
    if (transaction_ != nullptr) {
        transaction_->rollback();
        transaction_.reset();
        if (global_)
            globalStableIds_ = std::move(dragOriginalIds_);
        else if (const auto found = modelStableIds_.find(target_); found != modelStableIds_.end())
            *found->second = std::move(dragOriginalIds_);
        dragOriginal_.reset();
        dragKeys_.clear();
    }
}

std::size_t EditorSession::flushOperations() {
    if (scene_ == nullptr || history_ == nullptr) {
        operations_.discard();
        return 0;
    }
    operations_.setTableResolver(
        [this](core::ModelId target, bool global) -> StableIdTable& { return stableIds(target, global); });
    return operations_.flush(*scene_, *history_);
}

} // namespace dayo::editor
