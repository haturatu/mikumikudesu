#include "editor/editor_operation.hpp"

#include "core/log.hpp"

namespace dayo::editor {

void EditorOperationQueue::push(EditorOperation operation) {
    operations_.push_back(std::move(operation));
}

void EditorOperationQueue::setStableIdTable(const StableIdTable& table) {
    stableIdTable_ = table;
    hasStableIdTable_ = true;
    externalStableIdTable_ = nullptr;
}

void EditorOperationQueue::setStableIdTable(StableIdTable& table) {
    stableIdTable_ = table;
    hasStableIdTable_ = true;
    externalStableIdTable_ = &table;
}

std::size_t EditorOperationQueue::flush(core::Scene& scene, core::CommandHistory& history) {
    std::size_t applied = 0;
    for (auto& operation : operations_) {
        if (std::holds_alternative<SetFrameOperation>(operation)) {
            const auto& value = std::get<SetFrameOperation>(operation);
            history.execute(scene, std::make_unique<core::SetFrameCommand>(scene.timeline().frame, value.frame));
            ++applied;
        } else if (std::holds_alternative<ReplaceMotionOperation>(operation)) {
            auto& value = std::get<ReplaceMotionOperation>(operation);
            const auto* before = scene.motion(value.target, value.global);
            core::VmdMotion snapshot = before != nullptr ? *before : core::VmdMotion{};
            history.execute(scene, std::make_unique<core::EditMotionCommand>(value.target, value.global,
                                                                             std::move(snapshot),
                                                                             std::move(value.motion), value.label));
            ++applied;
        } else {
            auto& value = std::get<MoveKeysOperation>(operation);
            const auto* before = scene.motion(value.target, value.global);
            if (before == nullptr) {
                log::warn("EditorOperationQueue: MoveKeysOperation has no motion target");
                continue;
            }
            auto document = core::toMotionDocument(*before);
            StableIdTable table = externalStableIdTable_ != nullptr ? *externalStableIdTable_ : stableIdTable_;
            if (!hasStableIdTable_)
                table.rebuild(document);

            std::vector<MotionKeyId> ids = value.keys;
            if (ids.empty()) {
                if (value.track < 0 || value.track >= 6) {
                    log::warn("EditorOperationQueue: MoveKeysOperation has invalid track ", value.track);
                    continue;
                }
                const auto track = static_cast<core::MotionTrack>(value.track);
                ids.reserve(value.stableIds.size());
                for (const auto stableId : value.stableIds)
                    ids.push_back({track, stableId});
            }
            std::vector<core::MotionKeyRef> refs;
            refs.reserve(ids.size());
            for (const auto& id : ids) {
                if (id.stableId == 0)
                    continue;
                const auto index = table.resolve(document, id);
                if (index.has_value())
                    refs.push_back({id.track, *index});
            }
            if (refs.empty()) {
                log::warn("EditorOperationQueue: MoveKeysOperation resolved no keys");
                continue;
            }
            core::MotionEditor::move(document, refs, value.frameDelta);
            if (externalStableIdTable_ != nullptr)
                externalStableIdTable_->notifyMoved(document, ids, value.frameDelta);
            auto after = core::toVmdMotion(std::move(document), before->modelName);
            history.execute(scene, std::make_unique<core::EditMotionCommand>(value.target, value.global, *before,
                                                                             std::move(after), "Move keys"));
            ++applied;
        }
    }
    operations_.clear();
    return applied;
}

void EditorOperationQueue::discard() noexcept {
    operations_.clear();
}

UndoTransaction::UndoTransaction(core::Scene& scene, core::CommandHistory& history, core::ModelId target, bool global,
                                 std::string label)
    : scene_(&scene), history_(&history), target_(target), global_(global), label_(std::move(label)) {
    const auto* current = scene_->motion(target_, global_);
    before_ = current != nullptr ? *current : core::VmdMotion{};
    current_ = *before_;
}

UndoTransaction::~UndoTransaction() {
    if (active_)
        rollback();
}

void UndoTransaction::dragTo(core::VmdMotion intermediate) {
    if (!active_)
        return;
    current_ = std::move(intermediate);
    // Preview only: bypass history so the drag coalesces into one entry.
    static_cast<void>(scene_->replaceMotion(*current_, target_, global_));
}

void UndoTransaction::commit() {
    if (!active_)
        return;
    active_ = false;
    if (!before_.has_value() || !current_.has_value())
        return;
    // Restore the pre-drag state, then push one coalesced command so
    // undo returns exactly to `before`.
    static_cast<void>(scene_->replaceMotion(*before_, target_, global_));
    history_->execute(scene_, std::make_unique<core::EditMotionCommand>(target_, global_, std::move(*before_),
                                                                        std::move(*current_), label_));
    before_.reset();
    current_.reset();
}

void UndoTransaction::rollback() noexcept {
    if (!active_)
        return;
    active_ = false;
    try {
        if (before_.has_value())
            static_cast<void>(scene_->replaceMotion(*before_, target_, global_));
    } catch (...) {
    }
    before_.reset();
    current_.reset();
}

} // namespace dayo::editor
