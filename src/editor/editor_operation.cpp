#include "editor/editor_operation.hpp"

#include "core/log.hpp"

namespace dayo::editor {

namespace {
class MaterialCommand final : public core::EditCommand {
  public:
    MaterialCommand(core::ModelId target, std::size_t material, core::MaterialEditorState before,
                    core::MaterialEditorState after, std::string label)
        : target_(target), material_(material), before_(std::move(before)), after_(std::move(after)),
          label_(std::move(label)) {}
    void apply(core::Scene& scene) override {
        set(scene, after_);
    }
    void undo(core::Scene& scene) override {
        set(scene, before_);
    }
    const char* name() const noexcept override {
        return label_.c_str();
    }

  private:
    void set(core::Scene& scene, const core::MaterialEditorState& state) {
        auto* model = scene.model(target_);
        if (!model || material_ >= model->materialSettings.size())
            return;
        model->materialSettings[material_] = state;
        scene.markDirty(core::DirtyFlag::material | core::DirtyFlag::effect);
    }
    core::ModelId target_{};
    std::size_t material_{};
    core::MaterialEditorState before_, after_;
    std::string label_;
};
class StableMotionCommand final : public core::EditCommand {
  public:
    StableMotionCommand(core::ModelId target, bool global, core::VmdMotion before, core::VmdMotion after,
                        std::string label, StableIdTable* table, StableIdTable beforeIds, StableIdTable afterIds)
        : motion_(target, global, std::move(before), std::move(after), std::move(label)), table_(table),
          beforeIds_(std::move(beforeIds)), afterIds_(std::move(afterIds)) {}
    void apply(core::Scene& scene) override {
        if (table_)
            *table_ = afterIds_;
        motion_.apply(scene);
    }
    void undo(core::Scene& scene) override {
        if (table_)
            *table_ = beforeIds_;
        motion_.undo(scene);
    }
    const char* name() const noexcept override {
        return motion_.name();
    }

  private:
    core::EditMotionCommand motion_;
    StableIdTable* table_{};
    StableIdTable beforeIds_;
    StableIdTable afterIds_;
};
} // namespace

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
        if (tableResolver_) {
            std::visit(
                [&](const auto& value) {
                    if constexpr (requires {
                                      value.target;
                                      value.global;
                                  })
                        setStableIdTable(tableResolver_(value.target, value.global));
                },
                operation);
        }
        if (std::holds_alternative<SetFrameOperation>(operation)) {
            const auto& value = std::get<SetFrameOperation>(operation);
            history.execute(scene, std::make_unique<core::SetFrameCommand>(scene.timeline().frame, value.frame));
            ++applied;
        } else if (std::holds_alternative<ReplaceMotionOperation>(operation)) {
            auto& value = std::get<ReplaceMotionOperation>(operation);
            const auto* before = scene.motion(value.target, value.global);
            core::VmdMotion snapshot = before != nullptr ? *before : core::VmdMotion{};
            auto beforeIds = externalStableIdTable_ ? *externalStableIdTable_ : stableIdTable_;
            auto afterIds = beforeIds;
            afterIds.rebuild(core::toMotionDocument(value.motion));
            history.execute(scene, std::make_unique<StableMotionCommand>(
                                       value.target, value.global, std::move(snapshot), std::move(value.motion),
                                       value.label, externalStableIdTable_, std::move(beforeIds), std::move(afterIds)));
            ++applied;
        } else if (std::holds_alternative<MoveKeysOperation>(operation)) {
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
                if (value.track < 0 || static_cast<std::size_t>(value.track) >= core::motionTrackCount) {
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
            auto beforeIds = table;
            core::MotionEditor::move(document, refs, value.frameDelta);
            if (externalStableIdTable_ != nullptr)
                externalStableIdTable_->notifyMoved(document, ids, value.frameDelta);
            else
                stableIdTable_.notifyMoved(document, ids, value.frameDelta);
            auto after = core::toVmdMotion(std::move(document), before->modelName);
            auto afterIds = externalStableIdTable_ ? *externalStableIdTable_ : stableIdTable_;
            afterIds.rebuild(core::toMotionDocument(after));
            history.execute(scene, std::make_unique<StableMotionCommand>(
                                       value.target, value.global, *before, std::move(after), "Move keys",
                                       externalStableIdTable_, std::move(beforeIds), std::move(afterIds)));
            ++applied;
        } else {
            auto& value = std::get<MaterialEditOperation>(operation);
            auto* model = scene.model(value.target);
            if (model && value.material < model->materialSettings.size()) {
                history.execute(scene, std::make_unique<MaterialCommand>(value.target, value.material,
                                                                         model->materialSettings[value.material],
                                                                         std::move(value.state), value.label));
                ++applied;
            }
        }
    }
    operations_.clear();
    return applied;
}

void EditorOperationQueue::discard() noexcept {
    operations_.clear();
}

UndoTransaction::UndoTransaction(core::Scene& scene, core::CommandHistory& history, core::ModelId target, bool global,
                                 std::string label, StableIdTable* stableIds)
    : scene_(&scene), history_(&history), target_(target), global_(global), label_(std::move(label)),
      stableIds_(stableIds) {
    if (stableIds_)
        beforeIds_ = *stableIds_;
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
    if (stableIds_ && beforeIds_) {
        history_->execute(*scene_, std::make_unique<StableMotionCommand>(target_, global_, std::move(*before_),
                                                                         std::move(*current_), label_, stableIds_,
                                                                         std::move(*beforeIds_), *stableIds_));
    } else {
        history_->execute(*scene_, std::make_unique<core::EditMotionCommand>(target_, global_, std::move(*before_),
                                                                             std::move(*current_), label_));
    }
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
        if (stableIds_ && beforeIds_)
            *stableIds_ = *beforeIds_;
    } catch (...) {
    }
    before_.reset();
    current_.reset();
}

} // namespace dayo::editor
