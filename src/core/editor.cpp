#include "core/editor.hpp"

#include <utility>

namespace dayo::core {

void CommandHistory::execute(Scene& scene, std::unique_ptr<EditCommand> command) {
    if (!command) return;
    command->apply(scene);
    undo_.push_back(std::move(command));
    redo_.clear();
}

bool CommandHistory::undo(Scene& scene) {
    if (undo_.empty()) return false;
    auto command = std::move(undo_.back());
    undo_.pop_back();
    command->undo(scene);
    redo_.push_back(std::move(command));
    return true;
}

bool CommandHistory::redo(Scene& scene) {
    if (redo_.empty()) return false;
    auto command = std::move(redo_.back());
    redo_.pop_back();
    command->apply(scene);
    undo_.push_back(std::move(command));
    return true;
}

void CommandHistory::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

void SetFrameCommand::set(Scene& scene, float frame) {
    scene.timeline().frame = frame;
    scene.markDirty(DirtyFlag::camera | DirtyFlag::geometry | DirtyFlag::lighting);
}

void SetFrameCommand::apply(Scene& scene) { set(scene, after_); }
void SetFrameCommand::undo(Scene& scene) { set(scene, before_); }

void SetRuntimeModeCommand::apply(Scene& scene) { scene.setRuntimeMode(after_); }
void SetRuntimeModeCommand::undo(Scene& scene) { scene.setRuntimeMode(before_); }

} // namespace dayo::core
