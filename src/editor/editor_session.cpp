#include "editor/editor_session.hpp"

namespace dayo::editor {

void EditorSession::setTarget(core::ModelId target, bool global) noexcept {
    target_ = target;
    global_ = global;
}

void EditorSession::beginKeyframeDrag(const std::string& label) {
    if (scene_ == nullptr || history_ == nullptr || transaction_ != nullptr)
        return;
    transaction_ = std::make_unique<UndoTransaction>(*scene_, *history_, target_, global_, label);
}

void EditorSession::updateKeyframeDrag(core::VmdMotion intermediate) {
    if (transaction_ != nullptr)
        transaction_->dragTo(std::move(intermediate));
}

void EditorSession::commitKeyframeDrag() {
    if (transaction_ != nullptr) {
        transaction_->commit();
        transaction_.reset();
    }
}

void EditorSession::cancelKeyframeDrag() noexcept {
    if (transaction_ != nullptr) {
        transaction_->rollback();
        transaction_.reset();
    }
}

std::size_t EditorSession::flushOperations() {
    if (scene_ == nullptr || history_ == nullptr) {
        operations_.discard();
        return 0;
    }
    operations_.setStableIdTable(stableIds_);
    return operations_.flush(*scene_, *history_);
}

} // namespace dayo::editor
